#ifndef GLYPH_MANIFEST_H
#define GLYPH_MANIFEST_H

#include <stddef.h>
#include <stdbool.h>

typedef enum { GLYPH_SOURCE_ARCHIVE = 0, GLYPH_SOURCE_FILES } glyph_source_type_t;

typedef struct {
    char *url;
    char *sha256;
    char *name;
} glyph_source_file_t;

typedef struct {
    glyph_source_type_t type;
    /* archive fields (type == GLYPH_SOURCE_ARCHIVE) */
    char *url;
    char *sha256;
    char *format;   /* e.g. "zip" */
    /* files fields (type == GLYPH_SOURCE_FILES) */
    glyph_source_file_t *files;  /* array of n_files */
    size_t               n_files;
} glyph_source_t;

typedef struct {
    int    strip_components;
    char **include;   /* NULL-terminated array of glob patterns */
    char **exclude;   /* NULL-terminated array of glob patterns */
} glyph_install_t;

typedef struct {
    char            *id;
    char            *name;
    char            *author;
    char            *license;
    char            *category;
    char            *description;
    char            *version;
    int              revision;
    glyph_source_t   source;
    glyph_install_t  install;
    char            *homepage;
    char           **tags;        /* NULL-terminated */
} glyph_font_t;

typedef struct {
    char          *version;
    char          *last_updated;
    int            count;
    char          *signature_fingerprint;
    glyph_font_t  *fonts;         /* array of n_fonts */
    size_t         n_fonts;
} glyph_catalog_t;

/* Parse catalog from a NUL-terminated JSON string. Returns 0 on success. */
int  glyph_catalog_parse(const char *json, glyph_catalog_t *out);
/* Load + parse from file path. */
int  glyph_catalog_load_file(const char *path, glyph_catalog_t *out);
void glyph_catalog_free(glyph_catalog_t *cat);
/* Find a font by id (linear). Returns NULL if not found. */
const glyph_font_t *glyph_catalog_find(const glyph_catalog_t *cat, const char *id);

#endif /* GLYPH_MANIFEST_H */
