#ifndef GLYPH_DOWNLOAD_H
#define GLYPH_DOWNLOAD_H

#include <stddef.h>
#include <stdbool.h>

/* Returns true iff url uses an allowed scheme (https). */
bool glyph_url_is_allowed(const char *url);

/* Download url to dest_path (file). If resume is true and a partial file exists,
   attempt HTTP Range resume. Prints a progress bar to stderr. Returns 0 on success,
   negative on error. */
int glyph_download_file(const char *url, const char *dest_path, bool resume);

/* Download url into a malloc'd buffer. *out_buf and *out_len set on success. */
int glyph_download_memory(const char *url, char **out_buf, size_t *out_len);

/* Like glyph_download_memory(), additionally reporting the HTTP response code
 * in *out_http_status (0 if no HTTP response was received, e.g. DNS/TLS
 * failure). Lets callers distinguish HTTP 404 from transport failures. */
int glyph_download_memory_status(const char *url, char **out_buf,
                                 size_t *out_len, long *out_http_status);

#endif /* GLYPH_DOWNLOAD_H */
