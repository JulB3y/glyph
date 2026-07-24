/*
 * src/manifest.c — catalog JSON parsing.
 *
 * Full implementation of the contract declared in include/manifest.h. Designed to
 * compile cleanly under:
 *   gcc -std=c99 -Wall -Wextra -Werror -Wstrict-prototypes -Wmissing-prototypes
 *
 * Notes:
 *   - Feature-test macros are defined FIRST so every system header sees them.
 *   - All cJSON lookups are case-sensitive. Missing or non-string fields yield a
 *     NULL pointer in the destination struct rather than a NULL dereference from
 *     cJSON_GetStringValue().
 *   - Every allocation failure unwinds partial state and returns -1; on success
 *     glyph_catalog_free() reclaims everything.
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
#include "manifest.h"
#include "util.h"

#include <cjson/cJSON.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * Internal helpers
 * ------------------------------------------------------------------------- */

/* Free a NULL-terminated char** produced by dup_string_array(). No-op for NULL. */
static void free_string_list(char **list)
{
    if (list == NULL) {
        return;
    }
    for (size_t i = 0; list[i] != NULL; i++) {
        free(list[i]);
    }
    free(list);
}

/* Free every heap allocation owned by a single font record. No-op for NULL. */
static void free_font(glyph_font_t *f)
{
    if (f == NULL) {
        return;
    }
    free(f->id);
    free(f->name);
    free(f->author);
    free(f->license);
    free(f->category);
    free(f->description);
    free(f->version);
    free(f->source.url);
    free(f->source.sha256);
    free(f->source.format);
    if (f->source.files != NULL) {
        for (size_t i = 0; i < f->source.n_files; i++) {
            free(f->source.files[i].url);
            free(f->source.files[i].sha256);
            free(f->source.files[i].name);
        }
        free(f->source.files);
    }
    free(f->homepage);
    free_string_list(f->install.include);
    free_string_list(f->install.exclude);
    free_string_list(f->tags);
}

/*
 * Duplicate an optional string field from a JSON object into *out. If obj is
 * NULL or the key is absent / non-string, *out is set to NULL and 0 returned.
 * Returns -1 on allocation failure (*out is set to NULL).
 */
static int dup_opt(const cJSON *obj, const char *key, char **out)
{
    if (out == NULL) {
        errno = EINVAL;
        return -1;
    }
    *out = NULL;
    if (obj == NULL) {
        return 0;
    }
    const char *s = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(obj, key));
    if (s == NULL) {
        return 0;
    }
    char *dup = glyph_strdup(s);
    if (dup == NULL) {
        return -1;
    }
    *out = dup;
    return 0;
}

/* Read an optional integer field, or default_val if absent / non-number. */
static int get_int(const cJSON *obj, const char *key, int default_val)
{
    if (obj == NULL) {
        return default_val;
    }
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (item != NULL && cJSON_IsNumber(item)) {
        return item->valueint;
    }
    return default_val;
}

/*
 * Build a malloc'd, NULL-terminated char** from a JSON string array. Returns
 * NULL when arr is NULL or not an array. Returns NULL on allocation failure
 * after freeing any partial work. An empty array yields a 1-element array whose
 * single slot is NULL (a valid empty, NULL-terminated list).
 */
static char **dup_string_array(const cJSON *arr)
{
    if (arr == NULL || !cJSON_IsArray(arr)) {
        return NULL;
    }
    int n = cJSON_GetArraySize(arr);
    if (n < 0) {
        n = 0;
    }
    char **out = malloc(((size_t)n + 1u) * sizeof(char *));
    if (out == NULL) {
        return NULL;
    }
    for (int i = 0; i < n; i++) {
        const char *s = cJSON_GetStringValue(cJSON_GetArrayItem(arr, i));
        if (s == NULL) {
            /* Non-string entry: leave a NULL slot. */
            out[i] = NULL;
        } else {
            char *dup = glyph_strdup(s);
            if (dup == NULL) {
                for (int j = 0; j < i; j++) {
                    free(out[j]);
                }
                free(out);
                return NULL;
            }
            out[i] = dup;
        }
    }
    out[n] = NULL;
    return out;
}

/*
 * Parse a JSON string-array field into *out, distinguishing "field absent"
 * (NULL, success) from "allocation failure" (-1). A non-empty array that yields
 * a NULL result is reported as -1 so callers can unwind.
 */
static int set_str_array(const cJSON *parent, const char *key, char ***out)
{
    if (out == NULL) {
        errno = EINVAL;
        return -1;
    }
    *out = NULL;
    const cJSON *arr = (parent != NULL)
        ? cJSON_GetObjectItemCaseSensitive(parent, key) : NULL;
    char **res = dup_string_array(arr);
    if (res == NULL && arr != NULL && cJSON_IsArray(arr) &&
        cJSON_GetArraySize(arr) > 0) {
        /* Non-empty source array but no result → allocation failure. */
        return -1;
    }
    *out = res;
    return 0;
}

/* Parse the optional "source" sub-object. Returns 0 on success, -1 on error. */
static int parse_source(const cJSON *parent, glyph_source_t *out)
{
    if (out == NULL) {
        errno = EINVAL;
        return -1;
    }
    memset(out, 0, sizeof(*out));
    if (parent == NULL) {
        return 0;
    }
    const cJSON *src = cJSON_GetObjectItemCaseSensitive(parent, "source");
    if (src == NULL) {
        return 0;
    }

    const char *type_str = cJSON_GetStringValue(
        cJSON_GetObjectItemCaseSensitive(src, "type"));

    if (type_str != NULL && strcmp(type_str, "files") == 0) {
        out->type = GLYPH_SOURCE_FILES;
        const cJSON *farr = cJSON_GetObjectItemCaseSensitive(src, "files");
        if (farr == NULL || !cJSON_IsArray(farr) ||
            cJSON_GetArraySize(farr) <= 0) {
            glyph_log_err("files source has missing or empty files array");
            return -1;
        }
        int n = cJSON_GetArraySize(farr);
        out->files = calloc((size_t)n, sizeof(*out->files));
        if (out->files == NULL) {
            return -1;
        }
        out->n_files = (size_t)n;
        for (int i = 0; i < n; i++) {
            const cJSON *ent = cJSON_GetArrayItem(farr, i);
            if (dup_opt(ent, "url", &out->files[i].url) == -1 ||
                dup_opt(ent, "sha256", &out->files[i].sha256) == -1 ||
                dup_opt(ent, "name", &out->files[i].name) == -1) {
                goto fail;
            }
        }
        return 0;
    }

    if (type_str != NULL && strcmp(type_str, "archive") != 0) {
        glyph_log_err("unknown source type: %s", type_str);
        return -1;
    }

    /* archive (default) */
    out->type = GLYPH_SOURCE_ARCHIVE;
    if (dup_opt(src, "url", &out->url) == -1 ||
        dup_opt(src, "sha256", &out->sha256) == -1 ||
        dup_opt(src, "format", &out->format) == -1) {
        goto fail;
    }
    return 0;

fail:
    free(out->url);
    free(out->sha256);
    free(out->format);
    if (out->files != NULL) {
        for (size_t i = 0; i < out->n_files; i++) {
            free(out->files[i].url);
            free(out->files[i].sha256);
            free(out->files[i].name);
        }
        free(out->files);
    }
    memset(out, 0, sizeof(*out));
    return -1;
}

/* Parse the optional "install" sub-object. Returns 0 on success, -1 on OOM. */
static int parse_install(const cJSON *parent, glyph_install_t *out)
{
    if (out == NULL) {
        errno = EINVAL;
        return -1;
    }
    memset(out, 0, sizeof(*out));
    if (parent == NULL) {
        return 0;
    }
    const cJSON *inst = cJSON_GetObjectItemCaseSensitive(parent, "install");
    if (inst == NULL) {
        return 0;
    }
    out->strip_components = get_int(inst, "strip_components", 0);
    if (set_str_array(inst, "include", &out->include) == -1 ||
        set_str_array(inst, "exclude", &out->exclude) == -1) {
        free_string_list(out->include);
        free_string_list(out->exclude);
        memset(out, 0, sizeof(*out));
        return -1;
    }
    return 0;
}

/*
 * Parse a single font object into *out. On failure every field of *out is freed
 * and the record zeroed. A NULL object yields an all-zero font record (success),
 * so the caller can pass cJSON_GetArrayItem() results directly.
 */
static int parse_font(const cJSON *obj, glyph_font_t *out)
{
    if (out == NULL) {
        errno = EINVAL;
        return -1;
    }
    memset(out, 0, sizeof(*out));
    if (obj == NULL) {
        return 0;
    }

    if (dup_opt(obj, "id", &out->id) == -1 ||
        dup_opt(obj, "name", &out->name) == -1 ||
        dup_opt(obj, "author", &out->author) == -1 ||
        dup_opt(obj, "license", &out->license) == -1 ||
        dup_opt(obj, "category", &out->category) == -1 ||
        dup_opt(obj, "description", &out->description) == -1 ||
        dup_opt(obj, "version", &out->version) == -1) {
        goto fail;
    }

    out->revision = get_int(obj, "revision", 0);

    if (parse_source(obj, &out->source) == -1) {
        goto fail;
    }
    if (parse_install(obj, &out->install) == -1) {
        goto fail;
    }

    if (dup_opt(obj, "homepage", &out->homepage) == -1) {
        goto fail;
    }
    if (set_str_array(obj, "tags", &out->tags) == -1) {
        goto fail;
    }
    return 0;

fail:
    free_font(out);
    memset(out, 0, sizeof(*out));
    return -1;
}

/* ---------------------------------------------------------------------------
 * Catalog API (declared in include/manifest.h)
 * ------------------------------------------------------------------------- */

int glyph_catalog_parse(const char *json, glyph_catalog_t *out)
{
    if (out == NULL) {
        errno = EINVAL;
        return -1;
    }
    memset(out, 0, sizeof(*out));

    if (json == NULL) {
        errno = EINVAL;
        return -1;
    }

    cJSON *root = cJSON_Parse(json);
    if (root == NULL) {
        glyph_log_err("failed to parse catalog JSON");
        return -1;
    }

    int rc = 0;

    /* meta (every field optional). */
    const cJSON *meta = cJSON_GetObjectItemCaseSensitive(root, "meta");
    if (meta != NULL) {
        if (dup_opt(meta, "version", &out->version) == -1 ||
            dup_opt(meta, "last_updated", &out->last_updated) == -1 ||
            dup_opt(meta, "signature_fingerprint", &out->signature_fingerprint) == -1) {
            rc = -1;
            goto done;
        }
    }

    /* fonts array — allocate only when non-empty. */
    const cJSON *fonts = cJSON_GetObjectItemCaseSensitive(root, "fonts");
    int n = 0;
    if (fonts != NULL && cJSON_IsArray(fonts)) {
        n = cJSON_GetArraySize(fonts);
        if (n < 0) {
            n = 0;
        }
    }
    out->n_fonts = (size_t)n;

    if (n > 0) {
        out->fonts = malloc((size_t)n * sizeof(*out->fonts));
        if (out->fonts == NULL) {
            rc = -1;
            goto done;
        }
        for (int i = 0; i < n; i++) {
            memset(&out->fonts[i], 0, sizeof(out->fonts[i]));
        }
        for (int i = 0; i < n; i++) {
            const cJSON *fobj = cJSON_GetArrayItem(fonts, i);
            if (parse_font(fobj, &out->fonts[i]) == -1) {
                /* Free this partial font plus everything parsed before it. */
                for (int j = 0; j <= i; j++) {
                    free_font(&out->fonts[j]);
                }
                free(out->fonts);
                out->fonts = NULL;
                out->n_fonts = 0;
                rc = -1;
                goto done;
            }
        }
    }

    /* count defaults to the fonts-array length when meta omits it. */
    const cJSON *cnt = (meta != NULL)
        ? cJSON_GetObjectItemCaseSensitive(meta, "count") : NULL;
    if (cnt != NULL && cJSON_IsNumber(cnt)) {
        out->count = cnt->valueint;
    } else {
        out->count = n;
    }

done:
    cJSON_Delete(root);
    if (rc == -1) {
        free(out->version);
        free(out->last_updated);
        free(out->signature_fingerprint);
        memset(out, 0, sizeof(*out));
    }
    return rc;
}

int glyph_catalog_load_file(const char *path, glyph_catalog_t *out)
{
    if (path == NULL || out == NULL) {
        errno = EINVAL;
        return -1;
    }
    char *buf = NULL;
    if (glyph_read_file(path, &buf, NULL) == -1) {
        return -1;
    }
    int rc = glyph_catalog_parse(buf, out);
    free(buf);
    return rc;
}

void glyph_catalog_free(glyph_catalog_t *cat)
{
    if (cat == NULL) {
        return;
    }
    free(cat->version);
    free(cat->last_updated);
    free(cat->signature_fingerprint);
    for (size_t i = 0; i < cat->n_fonts; i++) {
        free_font(&cat->fonts[i]);
    }
    free(cat->fonts);
    memset(cat, 0, sizeof(*cat));
}

const glyph_font_t *glyph_catalog_find(const glyph_catalog_t *cat, const char *id)
{
    if (cat == NULL || id == NULL || cat->fonts == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < cat->n_fonts; i++) {
        if (cat->fonts[i].id != NULL && strcmp(cat->fonts[i].id, id) == 0) {
            return &cat->fonts[i];
        }
    }
    return NULL;
}
