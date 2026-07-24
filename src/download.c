/*
 * src/download.c — libcurl-backed HTTPS downloads with progress + resume.
 *
 * Full implementation of the contract declared in include/download.h. Designed
 * to compile cleanly under:
 *   gcc -std=c99 -Wall -Wextra -Werror -Wstrict-prototypes -Wmissing-prototypes
 *
 * Notes:
 *   - Feature-test macros are defined FIRST so every system header sees them.
 *   - HTTPS-only policy is enforced twice: once in glyph_url_is_allowed() and
 *     again via CURLOPT_PROTOCOLS / CURLOPT_REDIR_PROTOCOLS (defense in depth).
 *   - CURLOPT_PROTOCOLS_STR (curl >= 7.85.0) is preferred where available to
 *     avoid the deprecation warning attached to the bitmask form on newer
 *     libcurl; we fall back to the bitmask on older builds (>= 7.68).
 *   - Every exit path runs curl_easy_cleanup() and closes/frees whatever was
 *     opened; the memory variant frees its buffer on the error path.
 */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700
#endif

#include "glyph.h"
#include "download.h"
#include "util.h"

#include <curl/curl.h>

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>   /* strncasecmp */
#include <sys/stat.h>

/* ---------------------------------------------------------------------------
 * Internal types
 * ------------------------------------------------------------------------- */

/* Growable byte sink used by glyph_download_memory(). */
struct dlmem {
    char  *data;  /* malloc'd; never NULL on the success path when len > 0 */
    size_t len;   /* valid bytes stored                       */
    size_t cap;   /* allocated capacity (>= len)              */
};

/* ---------------------------------------------------------------------------
 * Internal helpers
 * ------------------------------------------------------------------------- */

/* Render a byte count into a compact human-readable string (e.g. "1.23 MiB").
 * Trailing buffer always NUL-terminated; safe for dltotal == 0. */
static void format_bytes(curl_off_t bytes, char *buf, size_t buflen)
{
    if (bytes >= ((curl_off_t)1 << 30)) {
        snprintf(buf, buflen, "%.2f GiB", (double)bytes / (double)((curl_off_t)1 << 30));
    } else if (bytes >= ((curl_off_t)1 << 20)) {
        snprintf(buf, buflen, "%.2f MiB", (double)bytes / (double)((curl_off_t)1 << 20));
    } else if (bytes >= ((curl_off_t)1 << 10)) {
        snprintf(buf, buflen, "%.2f KiB", (double)bytes / (double)((curl_off_t)1 << 10));
    } else {
        snprintf(buf, buflen, "%lld B", (long long)bytes);
    }
}

/* CURLOPT_XFERINFOFUNCTION: emit a single-line, carriage-return-updated
 * progress report to stderr. Divides are guarded against dltotal == 0.
 * Returns 0 (never aborts the transfer). */
static int xferinfo_cb(void *clientp, curl_off_t dltotal, curl_off_t dlnow,
                       curl_off_t ultotal, curl_off_t ulnow)
{
    (void)clientp;
    (void)ultotal;
    (void)ulnow;

    char now_str[32];
    char tot_str[32];

    if (dltotal <= 0) {
        /* Total size unknown (chunked / no Content-Length): show bytes only. */
        format_bytes(dlnow, now_str, sizeof now_str);
        fprintf(stderr, "  downloading... %s        \r", now_str);
    } else {
        curl_off_t have = (dlnow > dltotal) ? dltotal : dlnow;
        double pct = 100.0 * (double)have / (double)dltotal;
        format_bytes(dlnow, now_str, sizeof now_str);
        format_bytes(dltotal, tot_str, sizeof tot_str);
        fprintf(stderr, "  %5.1f%%  (%s / %s)        \r",
                pct, now_str, tot_str);
    }
    fflush(stderr);
    return 0;
}

/* CURLOPT_WRITEFUNCTION for glyph_download_file(): stream into a FILE*. */
static size_t write_file_cb(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    FILE *fp = (FILE *)userdata;
    size_t want = size * nmemb;
    if (want == 0) {
        return 0;
    }
    return fwrite(ptr, 1, want, fp);
}

/* CURLOPT_WRITEFUNCTION for glyph_download_memory(): append into a growing
 * malloc'd buffer. Returns the number of bytes consumed; returning less than
 * `want` aborts the transfer with CURLE_WRITE_ERROR. */
static size_t write_mem_cb(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    struct dlmem *m = (struct dlmem *)userdata;
    size_t want = size * nmemb;
    if (want == 0) {
        return 0;
    }

    if (m->len + want > m->cap) {
        size_t need = m->len + want;
        size_t ncap = (m->cap != 0) ? m->cap : 8192;
        while (ncap < need) {
            if (ncap > (SIZE_MAX / 2)) {
                ncap = need; /* overflow guard: just satisfy the request */
                break;
            }
            ncap *= 2;
        }
        char *nd = realloc(m->data, ncap);
        if (nd == NULL) {
            return 0; /* signal failure to libcurl */
        }
        m->data = nd;
        m->cap = ncap;
    }

    memcpy(m->data + m->len, ptr, want);
    m->len += want;
    return want;
}

/* Apply the security / timeout / UA options common to both download variants.
 * Returns 0 on success, -1 if any setopt fails (a sign of an unsupported
 * option on this libcurl build). */
static int set_common_opts(CURL *curl, const char *url)
{
    CURLcode c;

    c = curl_easy_setopt(curl, CURLOPT_URL, url);
    if (c != CURLE_OK) return -1;
    c = curl_easy_setopt(curl, CURLOPT_USERAGENT, GLYPH_USER_AGENT);
    if (c != CURLE_OK) return -1;
    c = curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    if (c != CURLE_OK) return -1;
    c = curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
    if (c != CURLE_OK) return -1;

    /* Defense in depth: never let libcurl speak or redirect to a non-https
     * scheme, even if the allow-list check above is bypassed. */
#if LIBCURL_VERSION_NUM >= 0x075500
    c = curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https");
    if (c != CURLE_OK) return -1;
    c = curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "https");
    if (c != CURLE_OK) return -1;
#else
    c = curl_easy_setopt(curl, CURLOPT_PROTOCOLS, (long)CURLPROTO_HTTPS);
    if (c != CURLE_OK) return -1;
    c = curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS, (long)CURLPROTO_HTTPS);
    if (c != CURLE_OK) return -1;
#endif

    c = curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L);
    if (c != CURLE_OK) return -1;
    /* Abort stalled transfers: < 1 KiB/s average over 60 s. */
    c = curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1024L);
    if (c != CURLE_OK) return -1;
    c = curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 60L);
    if (c != CURLE_OK) return -1;
    /* Don't use signals for DNS timeouts (avoid SIGPIPE/SIGALRM surprises). */
    c = curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    if (c != CURLE_OK) return -1;

    return 0;
}

/* Best-effort debug trace of one completed transfer. Never alters the
 * transfer result. */
static void debug_trace_curl(CURL *curl, const char *url)
{
    if (!glyph_debug_enabled()) {
        return;
    }
    long code = 0;
    curl_off_t bytes = 0;
    double dns = 0, conn = 0, tls = 0, ttfb = 0, total = 0;
    (void)curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    (void)curl_easy_getinfo(curl, CURLINFO_SIZE_DOWNLOAD_T, &bytes);
    (void)curl_easy_getinfo(curl, CURLINFO_NAMELOOKUP_TIME, &dns);
    (void)curl_easy_getinfo(curl, CURLINFO_CONNECT_TIME, &conn);
    (void)curl_easy_getinfo(curl, CURLINFO_APPCONNECT_TIME, &tls);
    (void)curl_easy_getinfo(curl, CURLINFO_STARTTRANSFER_TIME, &ttfb);
    (void)curl_easy_getinfo(curl, CURLINFO_TOTAL_TIME, &total);
    glyph_log_debug("%s status=%ld bytes=%lld dns=%.3fs conn=%.3fs tls=%.3fs ttfb=%.3fs total=%.3fs",
                    url, code, (long long)bytes, dns, conn, tls, ttfb, total);
}

/* ---------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */

bool glyph_url_is_allowed(const char *url)
{
    if (url == NULL) {
        return false;
    }

    /* The scheme is the run of characters before the first "://". */
    const char *sep = strstr(url, "://");
    if (sep == NULL) {
        return false;
    }
    size_t scheme_len = (size_t)(sep - url);
    if (scheme_len == 0) {
        return false;
    }

    for (size_t i = 0; GLYPH_ALLOWED_SCHEMES[i] != NULL; i++) {
        const char *allowed = GLYPH_ALLOWED_SCHEMES[i];
        size_t alen = strlen(allowed);
        if (alen == scheme_len && strncasecmp(url, allowed, alen) == 0) {
            return true;
        }
    }
    return false;
}

int glyph_download_file(const char *url, const char *dest_path, bool resume)
{
    if (url == NULL || dest_path == NULL) {
        glyph_log_err("download: url and dest_path must not be NULL");
        return -1;
    }
    if (!glyph_url_is_allowed(url)) {
        glyph_log_err("refusing non-HTTPS URL: %s", url);
        return -1;
    }

    CURL *curl = curl_easy_init();
    if (curl == NULL) {
        glyph_log_err("download: curl_easy_init failed");
        return -1;
    }

    /* Decide between resume (append + HTTP Range) and truncate. Only resume
     * when explicitly requested AND a non-empty regular file already exists. */
    const char *mode = "wb";
    curl_off_t resume_from = 0;
    bool do_resume = false;
    if (resume) {
        struct stat st;
        if (stat(dest_path, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0) {
            resume_from = (curl_off_t)st.st_size;
            do_resume = true;
            mode = "ab";
        }
    }

    FILE *fp = fopen(dest_path, mode);
    if (fp == NULL) {
        glyph_log_err("download: cannot open '%s' for writing: %s",
                      dest_path, strerror(errno));
        curl_easy_cleanup(curl);
        return -1;
    }

    int ret = 0;

    if (set_common_opts(curl, url) != 0) {
        glyph_log_err("download: libcurl option setup failed for '%s'", url);
        ret = -1;
    } else {
        /* Enable progress + route body into the file handle. */
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, xferinfo_cb);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_file_cb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);
        if (do_resume) {
            curl_easy_setopt(curl, CURLOPT_RESUME_FROM_LARGE, resume_from);
        }

        CURLcode rc = curl_easy_perform(curl);
        /* Terminate the carriage-return progress line on its own line. */
        fputc('\n', stderr);
        fflush(stderr);
        debug_trace_curl(curl, url);

        if (rc != CURLE_OK) {
            glyph_log_err("download failed for '%s': %s",
                          url, curl_easy_strerror(rc));
            /* Leave the partial file in place; a later call with resume=true
             * can pick up where we left off. */
            ret = -1;
        }
    }

    if (fclose(fp) == EOF && ret == 0) {
        glyph_log_err("download: error closing '%s': %s",
                      dest_path, strerror(errno));
        ret = -1;
    }
    curl_easy_cleanup(curl);
    return ret;
}

int glyph_download_memory(const char *url, char **out_buf, size_t *out_len)
{
    return glyph_download_memory_status(url, out_buf, out_len, NULL);
}

int glyph_download_memory_status(const char *url, char **out_buf,
                                 size_t *out_len, long *out_http_status)
{
    if (out_buf == NULL || out_len == NULL) {
        glyph_log_err("download: out_buf and out_len must not be NULL");
        return -1;
    }
    *out_buf = NULL;
    *out_len = 0;
    if (out_http_status != NULL) {
        *out_http_status = 0;
    }

    if (url == NULL) {
        glyph_log_err("download: url must not be NULL");
        return -1;
    }
    if (!glyph_url_is_allowed(url)) {
        glyph_log_err("refusing non-HTTPS URL: %s", url);
        return -1;
    }

    CURL *curl = curl_easy_init();
    if (curl == NULL) {
        glyph_log_err("download: curl_easy_init failed");
        return -1;
    }

    struct dlmem mem;
    mem.data = NULL;
    mem.len = 0;
    mem.cap = 0;

    int ret = 0;

    if (set_common_opts(curl, url) != 0) {
        glyph_log_err("download: libcurl option setup failed for '%s'", url);
        ret = -1;
    } else {
        /* In-memory fetch: keep stderr clean (NOPROGRESS=1). No resume. */
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 1L);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_mem_cb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &mem);

        CURLcode rc = curl_easy_perform(curl);
        debug_trace_curl(curl, url);
        /* Available even when FAILONERROR aborted the transfer (e.g. 404);
         * stays 0 when no HTTP response arrived at all. */
        if (out_http_status != NULL) {
            long code = 0;
            if (curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code) ==
                    CURLE_OK &&
                code > 0) {
                *out_http_status = code;
            }
        }
        if (rc != CURLE_OK) {
            glyph_log_err("download failed for '%s': %s",
                          url, curl_easy_strerror(rc));
            free(mem.data);
            mem.data = NULL;
            ret = -1;
        } else {
            /* Guarantee one extra byte for a NUL terminator so callers that
             * treat the buffer as text are safe. *out_len reports the real
             * byte count, which may legitimately be zero. */
            if (mem.cap < mem.len + 1) {
                char *nd = realloc(mem.data, mem.len + 1);
                if (nd == NULL) {
                    glyph_log_err("download: out of memory finalizing buffer");
                    free(mem.data);
                    mem.data = NULL;
                    ret = -1;
                } else {
                    mem.data = nd;
                    mem.cap = mem.len + 1;
                }
            }
            if (ret == 0) {
                mem.data[mem.len] = '\0';
                *out_buf = mem.data;
                *out_len = mem.len;
            }
        }
    }

    curl_easy_cleanup(curl);
    return ret;
}
