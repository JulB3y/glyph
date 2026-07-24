/*
 * src/util.c — path resolution, locks, string helpers.
 *
 * Full implementation of the contract declared in include/util.h. Designed to
 * compile cleanly under:
 *   gcc -std=c99 -Wall -Wextra -Werror -Wstrict-prototypes -Wmissing-prototypes
 *
 * Notes:
 *   - Feature-test macros are defined FIRST so every system header sees them.
 *   - strdup()/strndup() are implemented locally (no reliance on libc's
 *     _POSIX_C_SOURCE-gated declarations, which avoids FTW macro pitfalls).
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
#include "util.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

/* Defined in glyph.h as `extern const char *const GLYPH_ALLOWED_SCHEMES[];`. */
const char *const GLYPH_ALLOWED_SCHEMES[] = { "https", NULL };

/* Opaque lock handle. */
struct glyph_lock {
    int fd;
};

/* ---------------------------------------------------------------------------
 * Internal helpers
 * ------------------------------------------------------------------------- */

/*
 * Join $HOME with the given suffix (suffix must start with '/'). Returns a
 * malloc'd string or NULL (errno = ENOENT) if HOME is unset/empty.
 */
static char *join_home(const char *suffix)
{
    const char *home = getenv("HOME");
    if (home == NULL || home[0] == '\0') {
        errno = ENOENT;
        return NULL;
    }
    size_t hlen = strlen(home);
    size_t slen = strlen(suffix);
    char *out = malloc(hlen + slen + 1);
    if (out == NULL) {
        return NULL;
    }
    memcpy(out, home, hlen);
    memcpy(out + hlen, suffix, slen);
    out[hlen + slen] = '\0';
    return out;
}

/*
 * Resolve an XDG environment variable. If unset or empty, fall back to the
 * $HOME-anchored default suffix.
 */
static char *xdg_resolve(const char *env_var, const char *default_suffix)
{
    const char *val = getenv(env_var);
    if (val != NULL && val[0] != '\0') {
        return glyph_strdup(val);
    }
    return join_home(default_suffix);
}

/* ---------------------------------------------------------------------------
 * XDG base directories
 * ------------------------------------------------------------------------- */

char *glyph_xdg_data_home(void)
{
    return xdg_resolve("XDG_DATA_HOME", "/.local/share");
}

char *glyph_xdg_cache_home(void)
{
    return xdg_resolve("XDG_CACHE_HOME", "/.cache");
}

char *glyph_xdg_config_home(void)
{
    return xdg_resolve("XDG_CONFIG_HOME", "/.config");
}

/* ---------------------------------------------------------------------------
 * Composed glyph paths
 * ------------------------------------------------------------------------- */

char *glyph_path_fonts_root(void)
{
    char *data = glyph_xdg_data_home();
    if (data == NULL) {
        return NULL;
    }
    char *out = glyph_path_join(data, "fonts");
    free(data);
    return out;
}

char *glyph_path_font_dir(const char *id, const char *version)
{
    if (id == NULL || version == NULL) {
        errno = EINVAL;
        return NULL;
    }
    char *root = glyph_path_fonts_root();
    if (root == NULL) {
        return NULL;
    }
    char *tmp = glyph_path_join(root, id);
    free(root);
    if (tmp == NULL) {
        return NULL;
    }
    char *out = glyph_path_join(tmp, version);
    free(tmp);
    return out;
}

char *glyph_path_glyph_data_root(void)
{
    char *data = glyph_xdg_data_home();
    if (data == NULL) {
        return NULL;
    }
    char *out = glyph_path_join(data, "glyph");
    free(data);
    return out;
}

char *glyph_path_catalog_cache(void)
{
    char *cache = glyph_xdg_cache_home();
    if (cache == NULL) {
        return NULL;
    }
    char *tmp = glyph_path_join(cache, "glyph");
    free(cache);
    if (tmp == NULL) {
        return NULL;
    }
    char *out = glyph_path_join(tmp, "catalog.json");
    free(tmp);
    return out;
}

char *glyph_path_catalog_sig_cache(void)
{
    char *cache = glyph_xdg_cache_home();
    if (cache == NULL) {
        return NULL;
    }
    char *tmp = glyph_path_join(cache, "glyph");
    free(cache);
    if (tmp == NULL) {
        return NULL;
    }
    char *out = glyph_path_join(tmp, "catalog.json.sig2");
    free(tmp);
    return out;
}

char *glyph_path_installed_db(void)
{
    char *root = glyph_path_glyph_data_root();
    if (root == NULL) {
        return NULL;
    }
    char *out = glyph_path_join(root, "installed.json");
    free(root);
    return out;
}

char *glyph_path_lock_file(void)
{
    char *root = glyph_path_glyph_data_root();
    if (root == NULL) {
        return NULL;
    }
    char *out = glyph_path_join(root, ".lock");
    free(root);
    return out;
}

char *glyph_path_release_tag(void)
{
    char *cache = glyph_xdg_cache_home();
    if (cache == NULL) {
        return NULL;
    }
    char *tmp = glyph_path_join(cache, "glyph");
    free(cache);
    if (tmp == NULL) {
        return NULL;
    }
    char *out = glyph_path_join(tmp, "release-tag");
    free(tmp);
    return out;
}

char *glyph_read_release_tag(void)
{
    char *path = glyph_path_release_tag();
    if (path == NULL) {
        return NULL;
    }
    char *buf = NULL;
    size_t len = 0;
    int rc = glyph_read_file(path, &buf, &len);
    free(path);
    if (rc != 0) {
        return NULL; /* missing sidecar is not an error */
    }
    /* Trim surrounding whitespace (the file is written as "<tag>\n"). */
    while (len > 0 && isspace((unsigned char)buf[len - 1])) {
        buf[--len] = '\0';
    }
    size_t start = 0;
    while (buf[start] != '\0' && isspace((unsigned char)buf[start])) {
        start++;
    }
    if (start > 0) {
        memmove(buf, buf + start, len - start + 1);
    }
    if (buf[0] == '\0') {
        free(buf);
        return NULL;
    }
    return buf;
}

int glyph_write_release_tag(const char *tag)
{
    if (tag == NULL || tag[0] == '\0') {
        errno = EINVAL;
        return -1;
    }
    char *path = glyph_path_release_tag();
    if (path == NULL) {
        return -1;
    }
    /* The cache dir may not exist yet on a fresh install. */
    char *dup = glyph_strdup(path);
    if (dup != NULL) {
        (void)glyph_mkdir_p(dirname(dup), 0755);
        free(dup);
    }
    size_t n = strlen(tag);
    char *buf = malloc(n + 2); /* tag + '\n' */
    if (buf == NULL) {
        free(path);
        return -1;
    }
    memcpy(buf, tag, n);
    buf[n] = '\n';
    int rc = glyph_write_file(path, buf, n + 1, 0644);
    int saved_errno = errno;
    free(buf);
    free(path);
    errno = saved_errno;
    return rc;
}

bool glyph_catalog_cache_is_current(const char *tag)
{
    if (tag == NULL || tag[0] == '\0') {
        return false;
    }
    char *cached = glyph_read_release_tag();
    if (cached == NULL) {
        return false;
    }
    bool same = (strcmp(cached, tag) == 0);
    free(cached);
    if (!same) {
        return false;
    }
    char *cpath = glyph_path_catalog_cache();
    char *spath = glyph_path_catalog_sig_cache();
    bool ok = false;
    if (cpath != NULL && spath != NULL) {
        struct stat st;
        ok = (stat(cpath, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0 &&
              stat(spath, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0);
    }
    free(cpath);
    free(spath);
    return ok;
}

/* ---------------------------------------------------------------------------
 * mkdir -p
 * ------------------------------------------------------------------------- */

int glyph_mkdir_p(const char *path, mode_t mode)
{
    if (path == NULL || path[0] == '\0') {
        errno = EINVAL;
        return -1;
    }

    size_t len = strlen(path);
    char *copy = malloc(len + 1);
    if (copy == NULL) {
        return -1;
    }
    memcpy(copy, path, len + 1);

    /* Walk every path separator and mkdir the prefix up to that point. The
     * final component is handled when copy[i] == '\0' at i == len. */
    for (size_t i = 1; i <= len; i++) {
        if (copy[i] != '/' && copy[i] != '\0') {
            continue;
        }
        /* Skip the leading slash of an absolute path — never mkdir "/". */
        if (i == 1 && copy[0] == '/') {
            continue;
        }
        char saved = copy[i];
        copy[i] = '\0';
        if (mkdir(copy, mode) == -1 && errno != EEXIST) {
            int saved_errno = errno;
            free(copy);
            errno = saved_errno;
            return -1;
        }
        copy[i] = saved;
    }
    free(copy);
    return 0;
}

/* ---------------------------------------------------------------------------
 * File lock (flock-based, exclusive, non-blocking)
 * ------------------------------------------------------------------------- */

int glyph_lock_acquire(glyph_lock_t **out)
{
    if (out == NULL) {
        errno = EINVAL;
        return -1;
    }
    *out = NULL;

    char *root = glyph_path_glyph_data_root();
    if (root == NULL) {
        return -1;
    }
    if (glyph_mkdir_p(root, 0755) == -1) {
        int saved_errno = errno;
        free(root);
        errno = saved_errno;
        return -1;
    }
    char *lock_path = glyph_path_join(root, ".lock");
    free(root);
    if (lock_path == NULL) {
        return -1;
    }

    int fd = open(lock_path, O_CREAT | O_RDWR, 0644);
    free(lock_path);
    if (fd == -1) {
        return -1;
    }

    if (flock(fd, LOCK_EX | LOCK_NB) == -1) {
        int saved_errno = errno;
        close(fd);
        errno = saved_errno;
        return -1; /* EWOULDBLOCK / EAGAIN if already held */
    }

    glyph_lock_t *lk = malloc(sizeof(*lk));
    if (lk == NULL) {
        int saved_errno = errno;
        close(fd);
        errno = saved_errno;
        return -1;
    }
    lk->fd = fd;
    *out = lk;
    return 0;
}

void glyph_lock_release(glyph_lock_t *lock)
{
    if (lock == NULL) {
        return;
    }
    if (lock->fd != -1) {
        /* Best-effort: ignore errors on cleanup. */
        (void)flock(lock->fd, LOCK_UN);
        (void)close(lock->fd);
    }
    free(lock);
}

/* ---------------------------------------------------------------------------
 * String helpers
 * ------------------------------------------------------------------------- */

char *glyph_strdup(const char *s)
{
    if (s == NULL) {
        errno = EINVAL;
        return NULL;
    }
    size_t n = strlen(s);
    char *out = malloc(n + 1);
    if (out == NULL) {
        return NULL;
    }
    memcpy(out, s, n + 1);
    return out;
}

char *glyph_strndup(const char *s, size_t n)
{
    if (s == NULL) {
        errno = EINVAL;
        return NULL;
    }
    size_t slen = 0;
    while (slen < n && s[slen] != '\0') {
        slen++;
    }
    char *out = malloc(slen + 1);
    if (out == NULL) {
        return NULL;
    }
    memcpy(out, s, slen);
    out[slen] = '\0';
    return out;
}

bool glyph_str_starts_with(const char *s, const char *prefix)
{
    if (s == NULL || prefix == NULL) {
        return false;
    }
    size_t plen = strlen(prefix);
    return strncmp(s, prefix, plen) == 0;
}

bool glyph_str_ends_with(const char *s, const char *suffix)
{
    if (s == NULL || suffix == NULL) {
        return false;
    }
    size_t slen = strlen(s);
    size_t suflen = strlen(suffix);
    if (suflen > slen) {
        return false;
    }
    return strcmp(s + slen - suflen, suffix) == 0;
}

char *glyph_path_join(const char *a, const char *b)
{
    if (a == NULL || b == NULL) {
        errno = EINVAL;
        return NULL;
    }

    size_t alen = strlen(a);
    size_t blen = strlen(b);

    /* Strip trailing '/' from a. */
    while (alen > 0 && a[alen - 1] == '/') {
        alen--;
    }
    /* Strip leading '/' from b. */
    size_t bstart = 0;
    while (bstart < blen && b[bstart] == '/') {
        bstart++;
    }
    size_t beff = blen - bstart;
    bool need_sep = (alen > 0 && beff > 0);

    char *out = malloc(alen + (need_sep ? 1u : 0u) + beff + 1u);
    if (out == NULL) {
        return NULL;
    }
    size_t pos = 0;
    memcpy(out + pos, a, alen);
    pos += alen;
    if (need_sep) {
        out[pos++] = '/';
    }
    memcpy(out + pos, b + bstart, beff);
    pos += beff;
    out[pos] = '\0';
    return out;
}

/* ---------------------------------------------------------------------------
 * Timestamps
 * ------------------------------------------------------------------------- */

char *glyph_timestamp_iso8601(void)
{
    time_t now = time(NULL);
    if (now == (time_t)-1) {
        return NULL;
    }
    struct tm tmv;
    if (gmtime_r(&now, &tmv) == NULL) {
        return NULL;
    }
    /* "YYYY-MM-DDTHH:MM:SSZ" = 20 chars + NUL = 21. */
    char *out = malloc(21);
    if (out == NULL) {
        return NULL;
    }
    if (strftime(out, 21, "%Y-%m-%dT%H:%M:%SZ", &tmv) == 0) {
        free(out);
        return NULL;
    }
    return out;
}

/* ---------------------------------------------------------------------------
 * Temp directory
 * ------------------------------------------------------------------------- */

char *glyph_mkdtemp(void)
{
    const char *tmp = getenv("TMPDIR");
    if (tmp == NULL || tmp[0] == '\0') {
        tmp = "/tmp";
    }

    char *tmpl = glyph_path_join(tmp, "glyph-XXXXXX");
    if (tmpl == NULL) {
        return NULL;
    }
    /* mkdtemp() rewrites XXXXXX in place and returns tmpl on success. */
    char *res = mkdtemp(tmpl);
    if (res == NULL) {
        int saved_errno = errno;
        free(tmpl);
        errno = saved_errno;
        return NULL;
    }
    /* mkdtemp creates with 0700 already on Linux; chmod for portability. */
    if (chmod(res, 0700) == -1) {
        int saved_errno = errno;
        (void)rmdir(res);
        free(tmpl);
        errno = saved_errno;
        return NULL;
    }
    return tmpl;
}

/* ---------------------------------------------------------------------------
 * Logging
 * ------------------------------------------------------------------------- */

static bool g_debug_on = false;

void glyph_debug_set(bool on) { g_debug_on = on; }
bool glyph_debug_enabled(void) { return g_debug_on; }

static void glyph_log_vemit(const char *prefix, const char *fmt, va_list ap)
{
    fputs("glyph:", stderr);
    if (prefix != NULL) {
        fputc(' ', stderr);
        fputs(prefix, stderr);
    }
    fputc(' ', stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
}

void glyph_log_err(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    glyph_log_vemit("error:", fmt, ap);
    va_end(ap);
}

void glyph_log_warn(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    glyph_log_vemit("warning:", fmt, ap);
    va_end(ap);
}

void glyph_log_info(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    glyph_log_vemit(NULL, fmt, ap);
    va_end(ap);
}

void glyph_log_debug(const char *fmt, ...)
{
    if (!g_debug_on) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    glyph_log_vemit("debug:", fmt, ap);
    va_end(ap);
}

double glyph_now_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

/* ---------------------------------------------------------------------------
 * File I/O
 * ------------------------------------------------------------------------- */

int glyph_read_file(const char *path, char **out_buf, size_t *out_len)
{
    if (path == NULL || out_buf == NULL) {
        errno = EINVAL;
        return -1;
    }
    *out_buf = NULL;
    if (out_len != NULL) {
        *out_len = 0;
    }

    FILE *fp = fopen(path, "rb");
    if (fp == NULL) {
        return -1;
    }

    /* Pre-size the buffer for regular files where fstat() reports a length. */
    size_t cap = 8192;
    struct stat st;
    if (fstat(fileno(fp), &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0) {
        cap = (size_t)st.st_size + 1u;
    }

    char *buf = malloc(cap);
    if (buf == NULL) {
        int saved_errno = errno;
        fclose(fp);
        errno = saved_errno;
        return -1;
    }

    size_t off = 0;
    for (;;) {
        if (off + 1u >= cap) {
            size_t ncap = cap * 2u;
            char *nb = realloc(buf, ncap);
            if (nb == NULL) {
                int saved_errno = errno;
                free(buf);
                fclose(fp);
                errno = saved_errno;
                return -1;
            }
            buf = nb;
            cap = ncap;
        }
        size_t got = fread(buf + off, 1u, cap - off - 1u, fp);
        off += got;
        if (got == 0u) {
            if (ferror(fp)) {
                int saved_errno = errno;
                free(buf);
                fclose(fp);
                errno = saved_errno;
                return -1;
            }
            break; /* EOF */
        }
    }

    buf[off] = '\0';
    if (fclose(fp) == EOF) {
        int saved_errno = errno;
        free(buf);
        errno = saved_errno;
        return -1;
    }
    *out_buf = buf;
    if (out_len != NULL) {
        *out_len = off;
    }
    return 0;
}

int glyph_write_file(const char *path, const void *buf, size_t len, mode_t mode)
{
    if (path == NULL || (buf == NULL && len > 0u)) {
        errno = EINVAL;
        return -1;
    }

    /* Stage writes in `<path>.tmp.<pid>` next to the destination so the final
     * rename() is atomic on the same filesystem. */
    pid_t pid = getpid();
    int n = snprintf(NULL, 0, "%s.tmp.%d", path, (int)pid);
    if (n < 0) {
        return -1;
    }
    char *tmp = malloc((size_t)n + 1u);
    if (tmp == NULL) {
        return -1;
    }
    snprintf(tmp, (size_t)n + 1u, "%s.tmp.%d", path, (int)pid);

    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, mode);
    if (fd == -1) {
        int saved_errno = errno;
        free(tmp);
        errno = saved_errno;
        return -1;
    }

    const char *p = (const char *)buf;
    size_t off = 0;
    while (off < len) {
        ssize_t w = write(fd, p + off, len - off);
        if (w == -1) {
            if (errno == EINTR) {
                continue;
            }
            int saved_errno = errno;
            close(fd);
            (void)unlink(tmp);
            free(tmp);
            errno = saved_errno;
            return -1;
        }
        off += (size_t)w;
    }

    if (fsync(fd) == -1) {
        int saved_errno = errno;
        close(fd);
        (void)unlink(tmp);
        free(tmp);
        errno = saved_errno;
        return -1;
    }
    if (close(fd) == -1) {
        int saved_errno = errno;
        (void)unlink(tmp);
        free(tmp);
        errno = saved_errno;
        return -1;
    }
    if (rename(tmp, path) == -1) {
        int saved_errno = errno;
        (void)unlink(tmp);
        free(tmp);
        errno = saved_errno;
        return -1;
    }
    /* rename() preserves tmp's mode, but open() applied the process umask.
     * Normalize to the caller's requested mode. */
    if (chmod(path, mode) == -1) {
        int saved_errno = errno;
        free(tmp);
        errno = saved_errno;
        return -1;
    }
    free(tmp);
    return 0;
}
