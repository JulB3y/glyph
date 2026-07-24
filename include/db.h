#ifndef GLYPH_DB_H
#define GLYPH_DB_H

#include <stddef.h>
#include <stdbool.h>
#include "manifest.h"

typedef struct {
    char  *id;
    char  *name;
    char  *version;
    int    revision;
    char  *install_date;
    char **files;        /* NULL-terminated absolute paths */
} glyph_installed_font_t;

typedef struct {
    glyph_installed_font_t *fonts;
    size_t                  n_fonts;
    int                     db_format;
    char                   *glyph_version;
} glyph_db_t;

/* Load installed.json (or empty DB if absent). Returns 0 on success. */
int  glyph_db_load(glyph_db_t *db);
/* Persist DB to installed.json (atomic write). Returns 0 on success. */
int  glyph_db_save(const glyph_db_t *db);
void glyph_db_free(glyph_db_t *db);

const glyph_installed_font_t *glyph_db_find(const glyph_db_t *db, const char *id);

/* Add or replace an entry. Takes ownership of files[] (will be freed on db_free).
   n_files is the count; a NULL terminator is constructed internally. */
int glyph_db_upsert(glyph_db_t *db, const glyph_font_t *font,
                    char **files, size_t n_files);

/* Remove an entry (does NOT delete files on disk — caller does that). Returns
   0 on success, -1 if not found. */
int glyph_db_remove(glyph_db_t *db, const char *id);

#endif /* GLYPH_DB_H */
