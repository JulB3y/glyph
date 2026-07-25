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
 *   - Progress reporting: TTY gets start/bar/finish; non-TTY gets one line;
 *     label==NULL or quiet suppresses everything.
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
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* ---------------------------------------------------------------------------
 * Internal types
 * ------------------------------------------------------------------------- */

/* Growable byte sink used by glyph_download_memory(). */
struct dlmem {
    char  *data;  /* malloc'd; never NULL on the success path when len > 0 */
    size_t len;   /* valid bytes stored                       */
    size_t cap;   /* allocated capacity (>= len)              */
};

/* Progress rendering context, passed as CURLOPT_XFERINFODATA. */
struct dl_progress {
    const char *label;
    bool active;      /* label != NULL && !quiet */
    bool show_bar;    /* active && progress */
    bool is_tty;
    int bar_cols;     /* inner bar columns, clamped [8,48] */
    struct timespec t_start;
    struct timespec t_last;
    curl_off_t bytes_last;
    double ema_speed; /* bytes/sec exponential moving average */
    int n_samples;
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

/* Terminal width of stderr; fallback 80. */
static int term_cols(void)
{
    struct winsize ws;
    if (ioctl(STDERR_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) {
        return (int)ws.ws_col;
    }
    return 80;
}

/* Format seconds as mm:ss or h:mm:ss (≥ 1 h). */
static void format_eta(double secs, char *buf, size_t buflen)
{
    if (secs < 0.0) {
        secs = 0.0;
    }
    unsigned long s = (unsigned long)(secs + 0.5);
    unsigned h = (unsigned)(s / 3600);
    unsigned m = (unsigned)((s % 3600) / 60);
    unsigned sec = (unsigned)(s % 60);
    if (h > 0) {
        snprintf(buf, buflen, "%u:%02u:%02u", h, m, sec);
    } else {
        snprintf(buf, buflen, "%02u:%02u", m, sec);
    }
}

/* Initialise the progress context from caller-supplied opts. */
static void progress_init(struct dl_progress *p, const glyph_dl_opts_t *opts)
{
    memset(p, 0, sizeof(*p));
    if (opts == NULL || opts->label == NULL || opts->quiet) {
        return;
    }
    p->label = opts->label;
    p->active = true;
    p->show_bar = opts->progress;
    p->is_tty = (isatty(STDERR_FILENO) != 0);
    if (p->show_bar && p->is_tty) {
        int bc = term_cols() - 55; /* reserve for text portion */
        if (bc < 8)  bc = 8;
        if (bc > 48) bc = 48;
        p->bar_cols = bc;
    }
    clock_gettime(CLOCK_MONOTONIC, &p->t_start);
    p->t_last = p->t_start;
}

/* Print the TTY start line. */
static void progress_start(const struct dl_progress *p)
{
    if (p->active && p->is_tty) {
        fprintf(stderr, "  downloading %s\n", p->label);
        fflush(stderr);
    }
}

/* Clear the in-place bar line (TTY only). */
static void progress_clear_bar(const struct dl_progress *p)
{
    if (p->active && p->is_tty && p->show_bar) {
        fputs("\r\033[K", stderr);
    }
}

/* Print the finish report (TTY: finish line; non-TTY: single line). */
static void progress_finish(const struct dl_progress *p, curl_off_t bytes)
{
    if (!p->active) {
        return;
    }
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    double dt = (double)(now.tv_sec - p->t_start.tv_sec) +
                (double)(now.tv_nsec - p->t_start.tv_nsec) * 1e-9;
    char sz[32];
    format_bytes(bytes, sz, sizeof sz);
    if (p->is_tty) {
        progress_clear_bar(p);
        fprintf(stderr, "  finished %s in %.1fs\n", sz, dt);
    } else {
        fprintf(stderr, "  downloaded %s (%s in %.1fs)\n", p->label, sz, dt);
    }
    fflush(stderr);
}

/* CURLOPT_XFERINFOFUNCTION: EMA speed tracking + TTY bar rendering.
 * Returns 0 (never aborts the transfer). */
static int xferinfo_cb(void *clientp, curl_off_t dltotal, curl_off_t dlnow,
                       curl_off_t ultotal, curl_off_t ulnow)
{
    (void)ultotal;
    (void)ulnow;
    struct dl_progress *p = (struct dl_progress *)clientp;
    if (!p->show_bar) {
        return 0;
    }

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    /* EMA speed update (α ≈ 0.4). First delta initialises; subsequent
     * deltas blend.  ETA suppressed until ≥ 2 samples. */
    if (p->n_samples > 0) {
        double dt = (double)(now.tv_sec - p->t_last.tv_sec) +
                    (double)(now.tv_nsec - p->t_last.tv_nsec) * 1e-9;
        if (dt > 0.001) {
            double inst = (double)(dlnow - p->bytes_last) / dt;
            if (p->n_samples == 1) {
                p->ema_speed = inst;
            } else {
                p->ema_speed = 0.4 * inst + 0.6 * p->ema_speed;
            }
        }
    }
    p->n_samples++;
    p->t_last = now;
    p->bytes_last = dlnow;

    if (!p->is_tty) {
        return 0; /* non-TTY: single line emitted at completion */
    }

    /* ---- TTY bar rendering ---- */
    char now_s[32], tot_s[32], spd_s[40], eta_s[32];
    format_bytes(dlnow, now_s, sizeof now_s);

    if (dltotal > 0) {
        curl_off_t have = (dlnow > dltotal) ? dltotal : dlnow;
        int pct = (int)(100.0 * (double)have / (double)dltotal);
        int filled = (int)((double)p->bar_cols * (double)have / (double)dltotal);
        if (filled > p->bar_cols) filled = p->bar_cols;
        if (filled < 0) filled = 0;
        format_bytes(dltotal, tot_s, sizeof tot_s);

        /* Build: \r[████░░░░] 100%  now / total  speed  eta mm:ss */
        char line[512];
        int pos = 0;
        line[pos++] = '\r';
        line[pos++] = '[';
        for (int i = 0; i < filled; i++) {
            line[pos++] = '\xe2'; line[pos++] = '\x96'; line[pos++] = '\x88'; /* █ */
        }
        for (int i = filled; i < p->bar_cols; i++) {
            line[pos++] = '\xe2'; line[pos++] = '\x96'; line[pos++] = '\x91'; /* ░ */
        }
        line[pos++] = ']';
        pos += snprintf(line + pos, sizeof(line) - (size_t)pos,
                        " %3d%%  %s / %s", pct, now_s, tot_s);
        if (p->ema_speed > 0.0) {
            format_bytes((curl_off_t)p->ema_speed, spd_s, sizeof spd_s);
            pos += snprintf(line + pos, sizeof(line) - (size_t)pos,
                            "  %s/s", spd_s);
            if (p->n_samples >= 2 && dltotal > dlnow) {
                double eta = (double)(dltotal - dlnow) / p->ema_speed;
                format_eta(eta, eta_s, sizeof eta_s);
                pos += snprintf(line + pos, sizeof(line) - (size_t)pos,
                                "  eta %s", eta_s);
            }
        }
        /* Clear any leftover from a previous longer line. */
        pos += snprintf(line + pos, sizeof(line) - (size_t)pos, "\033[K");
        fwrite(line, 1, (size_t)pos, stderr);
    } else {
        /* Unknown total: bytes + speed only, no bar/ETA. */
        if (p->ema_speed > 0.0) {
            format_bytes((curl_off_t)p->ema_speed, spd_s, sizeof spd_s);
            fprintf(stderr, "\r  %s  %s/s\033[K", now_s, spd_s);
        } else {
            fprintf(stderr, "\r  %s\033[K", now_s);
        }
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

int glyph_download_file(const char *url, const char *dest_path, bool resume,
                        const glyph_dl_opts_t *opts)
{
    if (url == NULL || dest_path == NULL) {
        glyph_log_err("download: url and dest_path must not be NULL");
        return -1;
    }
    if (!glyph_url_is_allowed(url)) {
        glyph_log_err("refusing non-HTTPS URL: %s", url);
        return -1;
    }

    struct dl_progress prog;
    progress_init(&prog, opts);

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
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_file_cb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);
        if (do_resume) {
            curl_easy_setopt(curl, CURLOPT_RESUME_FROM_LARGE, resume_from);
        }

        if (prog.show_bar) {
            curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
            curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, xferinfo_cb);
            curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &prog);
            progress_start(&prog);
        } else {
            curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 1L);
        }

        CURLcode rc = curl_easy_perform(curl);
        debug_trace_curl(curl, url);

        if (rc != CURLE_OK) {
            progress_clear_bar(&prog);
            glyph_log_err("download failed for '%s': %s",
                          url, curl_easy_strerror(rc));
            ret = -1;
        } else {
            curl_off_t total_dl = 0;
            (void)curl_easy_getinfo(curl, CURLINFO_SIZE_DOWNLOAD_T, &total_dl);
            progress_finish(&prog, total_dl);
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

int glyph_download_memory(const char *url, char **out_buf, size_t *out_len,
                          const glyph_dl_opts_t *opts)
{
    return glyph_download_memory_status(url, out_buf, out_len, NULL, opts);
}

int glyph_download_memory_status(const char *url, char **out_buf,
                                 size_t *out_len, long *out_http_status,
                                 const glyph_dl_opts_t *opts)
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

    struct dl_progress prog;
    progress_init(&prog, opts);

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
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_mem_cb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &mem);

        /* Enable progress only when label + progress are both set. */
        if (prog.show_bar) {
            curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
            curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, xferinfo_cb);
            curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &prog);
            progress_start(&prog);
        } else {
            curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 1L);
        }

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
            progress_clear_bar(&prog);
            glyph_log_err("download failed for '%s': %s",
                          url, curl_easy_strerror(rc));
            free(mem.data);
            mem.data = NULL;
            ret = -1;
        } else {
            curl_off_t total_dl = 0;
            (void)curl_easy_getinfo(curl, CURLINFO_SIZE_DOWNLOAD_T, &total_dl);
            progress_finish(&prog, total_dl);
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
