#ifndef GLYPH_EXTRACT_H
#define GLYPH_EXTRACT_H

#include <stddef.h>
#include <stdbool.h>
#include "manifest.h"

/* Glob match supporting '*' and '?'. */
bool glyph_glob_match(const char *pattern, const char *str);

/* Reject paths containing '..' components or absolute paths (traversal safety). */
bool glyph_path_is_safe(const char *path);

/* Extract archive_path (ZIP) into dest_dir (created if needed), applying install
   filters (include/exclude globs, strip_components). On success, *out_files is a
   malloc'd NULL-terminated array of malloc'd absolute extracted paths; *out_count
   is the number of entries. Caller frees each entry + the array. Returns 0 on
   success. */
int glyph_extract_zip(const char *archive_path, const char *dest_dir,
                      const glyph_install_t *install,
                      char ***out_files, size_t *out_count);

#endif /* GLYPH_EXTRACT_H */
