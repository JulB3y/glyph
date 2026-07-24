#ifndef GLYPH_UTIL_H
#define GLYPH_UTIL_H

#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>
#include <sys/types.h>

/* XDG base-directory resolution. Each returns a malloc'd string (caller frees),
   honoring $XDG_DATA_HOME / $XDG_CACHE_HOME / $XDG_CONFIG_HOME with defaults
   ~/.local/share , ~/.cache , ~/.config . */
char *glyph_xdg_data_home(void);
char *glyph_xdg_cache_home(void);
char *glyph_xdg_config_home(void);

/* Composed glyph paths (malloc'd, caller frees). */
char *glyph_path_fonts_root(void);                          /* <data>/fonts */
char *glyph_path_font_dir(const char *id, const char *version); /* <data>/fonts/<id>/<version> */
char *glyph_path_glyph_data_root(void);                     /* <data>/glyph */
char *glyph_path_catalog_cache(void);                       /* <cache>/glyph/catalog.json */
char *glyph_path_catalog_sig_cache(void);                   /* <cache>/glyph/catalog.json.sig2 */
char *glyph_path_release_tag(void);                         /* <cache>/glyph/release-tag */
char *glyph_path_installed_db(void);                        /* <data>/glyph/installed.json */
char *glyph_path_lock_file(void);                           /* <data>/glyph/.lock */

/* Release-tag sidecar (<cache>/glyph/release-tag, plain text "vX.Y.Z\n").
 * glyph_read_release_tag returns a malloc'd trimmed tag or NULL when the
 * sidecar is missing/empty. glyph_write_release_tag writes atomically (tmp +
 * rename), creating the cache directory as needed. */
char *glyph_read_release_tag(void);
int   glyph_write_release_tag(const char *tag);

/* True iff the cached release tag equals `tag` AND both cached catalog files
 * (catalog.json, catalog.json.sig2) exist and are non-empty. Used by
 * `index update` to skip re-downloading an unchanged release. */
bool  glyph_catalog_cache_is_current(const char *tag);

/* mkdir -p (mode 0755). Returns 0 on success, -1 on error (errno set). */
int glyph_mkdir_p(const char *path, mode_t mode);

/* Exclusive lock for mutating operations. Acquires flock(LOCK_EX | LOCK_NB)
   on the glyph lock file, creating <data>/glyph/ as needed. Returns 0 and sets
   *out on success; returns -1 (errno = EWOULDBLOCK-style) if already locked. */
typedef struct glyph_lock glyph_lock_t;
int  glyph_lock_acquire(glyph_lock_t **out);
void glyph_lock_release(glyph_lock_t *lock);

/* String helpers (errno-preserving where sensible). */
char *glyph_strdup(const char *s);
char *glyph_strndup(const char *s, size_t n);
bool  glyph_str_starts_with(const char *s, const char *prefix);
bool  glyph_str_ends_with(const char *s, const char *suffix);
/* Join two path segments with exactly one '/'. Returns malloc'd string. */
char *glyph_path_join(const char *a, const char *b);

/* Current UTC time as ISO-8601 "YYYY-MM-DDTHH:MM:SSZ". Returns malloc'd string. */
char *glyph_timestamp_iso8601(void);

/* Create a unique temp dir /tmp/glyph-XXXXXX (mode 0700). Returns malloc'd path. */
char *glyph_mkdtemp(void);

/* Logging to stderr. */
void glyph_log_err(const char *fmt, ...);
void glyph_log_warn(const char *fmt, ...);
void glyph_log_info(const char *fmt, ...);

/* Opt-in debug tracing (off by default). */
void glyph_debug_set(bool on);
bool glyph_debug_enabled(void);
void glyph_log_debug(const char *fmt, ...);

/* Monotonic clock, seconds (for phase timing). */
double glyph_now_sec(void);

/* Read entire file into a malloc'd buffer. *out_len receives size. */
int glyph_read_file(const char *path, char **out_buf, size_t *out_len);
/* Write buffer to file atomically (write to tmp then rename). mode applied. */
int glyph_write_file(const char *path, const void *buf, size_t len, mode_t mode);

#endif /* GLYPH_UTIL_H */
