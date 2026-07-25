#ifndef GLYPH_DOWNLOAD_H
#define GLYPH_DOWNLOAD_H

#include <stddef.h>
#include <stdbool.h>

/* Download reporting options.  Pass NULL for fully silent operation
 * (equivalent to {NULL, false, false}). */
typedef struct {
    const char *label;  /* NULL → fully silent (no start/bar/finish) */
    bool quiet;         /* suppress all download output */
    bool progress;      /* allow progress bar; meaningful only with label */
} glyph_dl_opts_t;

/* Returns true iff url uses an allowed scheme (https). */
bool glyph_url_is_allowed(const char *url);

/* Download url to dest_path (file). If resume is true and a partial file
 * exists, attempt HTTP Range resume.  When opts carries a non-NULL label
 * and quiet is false, a start/progress/finish report is written to stderr
 * (TTY: animated bar; non-TTY: single line on success).
 * Returns 0 on success, negative on error. */
int glyph_download_file(const char *url, const char *dest_path, bool resume,
                        const glyph_dl_opts_t *opts);

/* Download url into a malloc'd buffer. *out_buf and *out_len set on
 * success.  opts controls progress reporting (see above). */
int glyph_download_memory(const char *url, char **out_buf, size_t *out_len,
                          const glyph_dl_opts_t *opts);

/* Like glyph_download_memory(), additionally reporting the HTTP response
 * code in *out_http_status (0 if no HTTP response was received, e.g.
 * DNS/TLS failure).  Lets callers distinguish HTTP 404 from transport
 * failures. */
int glyph_download_memory_status(const char *url, char **out_buf,
                                 size_t *out_len, long *out_http_status,
                                 const glyph_dl_opts_t *opts);

#endif /* GLYPH_DOWNLOAD_H */
