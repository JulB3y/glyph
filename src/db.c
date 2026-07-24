/*
 * src/db.c — installed-state database (installed.json) load/save/query.
 *
 * Implements the contract declared in include/db.h. The on-disk format is the
 * JSON object described in pln-c-repo.md §2.1: a top-level object holding a
 * "meta" sub-object (glyph_version, db_format) and a "fonts" sub-object keyed
 * by font id (each value being {id,name,version,revision,install_date,files}).
 *
 * Designed to compile cleanly under:
 *   gcc -std=c99 -Wall -Wextra -Werror -Wstrict-prototypes -Wmissing-prototypes
 *
 * Notes:
 *   - Feature-test macros are defined FIRST so every system header sees them.
 *   - cJSON ownership: only roots obtained via cJSON_Parse / cJSON_CreateObject
 *     are released with cJSON_Delete. Strings produced by cJSON_PrintUnformatted
 *     are freed with the libc allocator (cJSON uses malloc underneath).
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
#include "db.h"
#include "util.h"

#include <cjson/cJSON.h>

#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * Internal helpers
 * ------------------------------------------------------------------------- */

/*
 * Release every heap-owned field of a single installed-font record and zero
 * the slot. Tolerates a partially-populated record (any field may be NULL).
 */
static void free_font_contents(glyph_installed_font_t *f)
{
    if (f == NULL) {
        return;
    }
    free(f->id);
    f->id = NULL;
    free(f->name);
    f->name = NULL;
    free(f->version);
    f->version = NULL;
    free(f->install_date);
    f->install_date = NULL;
    if (f->files != NULL) {
        for (size_t j = 0; f->files[j] != NULL; j++) {
            free(f->files[j]);
        }
        free(f->files);
        f->files = NULL;
    }
}

/*
 * Initialize a zeroed glyph_db_t as a valid empty database. idempotent on an
 * already-zeroed struct; never fails (glyph_version may end up NULL if the
 * strdup allocation fails, which all consumers tolerate).
 */
static void init_empty_db(glyph_db_t *db)
{
    db->fonts = NULL;
    db->n_fonts = 0;
    db->db_format = GLYPH_DB_FORMAT;
    db->glyph_version = glyph_strdup(GLYPH_VERSION);
}

/* ---------------------------------------------------------------------------
 * Load
 * ------------------------------------------------------------------------- */

/*
 * Read installed.json via glyph_path_installed_db() + glyph_read_file and parse
 * it into *db. If the file is absent (ENOENT) the database is initialized as
 * empty and 0 is returned. If the file exists but cannot be read or parsed,
 * the database is initialized as empty, a warning is logged on parse failure,
 * and -1 is returned (the struct is always left in a freeable/usable state).
 */
int glyph_db_load(glyph_db_t *db)
{
    if (db == NULL) {
        errno = EINVAL;
        return -1;
    }
    memset(db, 0, sizeof(*db));

    char *path = glyph_path_installed_db();
    if (path == NULL) {
        /* Could not even resolve the DB path — hand back a usable empty DB. */
        init_empty_db(db);
        return -1;
    }

    char *buf = NULL;
    int rf = glyph_read_file(path, &buf, NULL);
    int saved_errno = errno;
    free(path);
    errno = saved_errno;

    if (rf == -1) {
        if (saved_errno == ENOENT) {
            /* Fresh install: no installed.json yet. */
            init_empty_db(db);
            return 0;
        }
        /* Other read failure (permissions, I/O, ...). */
        init_empty_db(db);
        return -1;
    }

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (root == NULL) {
        glyph_log_warn("installed.json: parse failed; using empty database");
        init_empty_db(db);
        return -1;
    }

    /* --- meta block ----------------------------------------------------- */
    cJSON *meta = cJSON_GetObjectItem(root, "meta");
    if (cJSON_IsObject(meta)) {
        cJSON *gv = cJSON_GetObjectItem(meta, "glyph_version");
        if (cJSON_IsString(gv) && gv->valuestring != NULL) {
            db->glyph_version = glyph_strdup(gv->valuestring);
        }
        cJSON *df = cJSON_GetObjectItem(meta, "db_format");
        if (cJSON_IsNumber(df)) {
            db->db_format = (int)df->valuedouble;
        }
    }
    if (db->glyph_version == NULL) {
        db->glyph_version = glyph_strdup(GLYPH_VERSION);
    }
    if (db->db_format == 0) {
        db->db_format = GLYPH_DB_FORMAT;
    }

    /* --- fonts block ---------------------------------------------------- *
     * "fonts" is a JSON OBJECT keyed by id, not an array. Each value is a
     * record object. We pre-size the array from the object's child count. */
    cJSON *fonts = cJSON_GetObjectItem(root, "fonts");
    if (cJSON_IsObject(fonts)) {
        int n = cJSON_GetArraySize(fonts);
        if (n > 0) {
            db->fonts = calloc((size_t)n, sizeof(*db->fonts));
            if (db->fonts == NULL) {
                /* calloc failed: db is already a valid empty DB (n_fonts=0,
                 * fonts=NULL). Leave meta populated and report the failure. */
                cJSON_Delete(root);
                return -1;
            }

            cJSON *child = NULL;
            cJSON_ArrayForEach(child, fonts) {
                if (!cJSON_IsObject(child)) {
                    continue;
                }
                if (db->n_fonts >= (size_t)n) {
                    break; /* defensive: should never exceed child count */
                }
                glyph_installed_font_t *f = &db->fonts[db->n_fonts];

                /* id: explicit field preferred, fall back to the object key. */
                cJSON *jid = cJSON_GetObjectItem(child, "id");
                const char *id_src = (cJSON_IsString(jid) && jid->valuestring != NULL)
                                         ? jid->valuestring
                                         : ((child->string != NULL) ? child->string : "");
                f->id = glyph_strdup(id_src);

                cJSON *jname = cJSON_GetObjectItem(child, "name");
                if (cJSON_IsString(jname) && jname->valuestring != NULL) {
                    f->name = glyph_strdup(jname->valuestring);
                }

                cJSON *jver = cJSON_GetObjectItem(child, "version");
                if (cJSON_IsString(jver) && jver->valuestring != NULL) {
                    f->version = glyph_strdup(jver->valuestring);
                }

                cJSON *jrev = cJSON_GetObjectItem(child, "revision");
                if (cJSON_IsNumber(jrev)) {
                    f->revision = (int)jrev->valuedouble;
                }

                cJSON *jidate = cJSON_GetObjectItem(child, "install_date");
                if (cJSON_IsString(jidate) && jidate->valuestring != NULL) {
                    f->install_date = glyph_strdup(jidate->valuestring);
                }

                cJSON *jfiles = cJSON_GetObjectItem(child, "files");
                if (cJSON_IsArray(jfiles)) {
                    int nf = cJSON_GetArraySize(jfiles);
                    char **arr = malloc(((size_t)nf + 1u) * sizeof(*arr));
                    if (arr != NULL) {
                        size_t k = 0;
                        cJSON *jf = NULL;
                        cJSON_ArrayForEach(jf, jfiles) {
                            if (cJSON_IsString(jf) && jf->valuestring != NULL) {
                                char *dup = glyph_strdup(jf->valuestring);
                                if (dup != NULL) {
                                    arr[k++] = dup;
                                }
                                /* On dup failure we skip the entry rather
                                 * than abort the whole load (tolerate). */
                            }
                        }
                        arr[k] = NULL;
                        f->files = arr;
                    }
                    /* If the array malloc failed, f->files stays NULL. */
                }

                db->n_fonts++;
            }
        }
    }

    cJSON_Delete(root);
    return 0;
}

/* ---------------------------------------------------------------------------
 * Save
 * ------------------------------------------------------------------------- */

/*
 * Serialize *db to installed.json as an unformatted JSON object, creating the
 * parent directory first and writing atomically via glyph_write_file. Returns
 * 0 on success, -1 on any failure (allocation, mkdir, write).
 */
int glyph_db_save(const glyph_db_t *db)
{
    if (db == NULL) {
        errno = EINVAL;
        return -1;
    }

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return -1;
    }

    /* meta block */
    cJSON *meta = cJSON_CreateObject();
    if (meta == NULL) {
        cJSON_Delete(root);
        return -1;
    }
    const char *gv = (db->glyph_version != NULL) ? db->glyph_version : GLYPH_VERSION;
    int df = (db->db_format != 0) ? db->db_format : GLYPH_DB_FORMAT;
    cJSON_AddStringToObject(meta, "glyph_version", gv);
    cJSON_AddNumberToObject(meta, "db_format", (double)df);
    cJSON_AddItemToObject(root, "meta", meta);

    /* fonts block: object keyed by id */
    cJSON *fonts = cJSON_CreateObject();
    if (fonts == NULL) {
        cJSON_Delete(root);
        return -1;
    }
    for (size_t i = 0; i < db->n_fonts; i++) {
        const glyph_installed_font_t *f = &db->fonts[i];

        cJSON *fobj = cJSON_CreateObject();
        if (fobj == NULL) {
            cJSON_Delete(root);
            return -1;
        }
        cJSON_AddStringToObject(fobj, "id", f->id != NULL ? f->id : "");
        cJSON_AddStringToObject(fobj, "name", f->name != NULL ? f->name : "");
        cJSON_AddStringToObject(fobj, "version", f->version != NULL ? f->version : "");
        cJSON_AddNumberToObject(fobj, "revision", (double)f->revision);
        cJSON_AddStringToObject(fobj, "install_date",
                                f->install_date != NULL ? f->install_date : "");

        cJSON *farr = cJSON_CreateArray();
        if (farr == NULL) {
            cJSON_Delete(fobj);
            cJSON_Delete(root);
            return -1;
        }
        if (f->files != NULL) {
            for (size_t j = 0; f->files[j] != NULL; j++) {
                cJSON *s = cJSON_CreateString(f->files[j]);
                if (s != NULL) {
                    cJSON_AddItemToArray(farr, s);
                }
            }
        }
        cJSON_AddItemToObject(fobj, "files", farr);

        const char *key = (f->id != NULL) ? f->id : "";
        cJSON_AddItemToObject(fonts, key, fobj);
    }
    cJSON_AddItemToObject(root, "fonts", fonts);

    char *str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (str == NULL) {
        return -1;
    }

    /* Ensure the parent directory (<data>/glyph) exists. */
    char *data_root = glyph_path_glyph_data_root();
    if (data_root == NULL) {
        free(str);
        return -1;
    }
    int mkrc = glyph_mkdir_p(data_root, 0755);
    free(data_root);
    if (mkrc == -1) {
        free(str);
        return -1;
    }

    char *db_path = glyph_path_installed_db();
    if (db_path == NULL) {
        free(str);
        return -1;
    }
    int rc = glyph_write_file(db_path, str, strlen(str), 0644);
    free(db_path);
    free(str);
    return rc;
}

/* ---------------------------------------------------------------------------
 * Free
 * ------------------------------------------------------------------------- */

/*
 * Release all heap storage owned by *db and zero the struct. Safe to call on
 * a zeroed/partially-initialized struct (all fields tolerate NULL/0).
 */
void glyph_db_free(glyph_db_t *db)
{
    if (db == NULL) {
        return;
    }
    for (size_t i = 0; i < db->n_fonts; i++) {
        free_font_contents(&db->fonts[i]);
    }
    free(db->fonts);
    free(db->glyph_version);

    db->fonts = NULL;
    db->n_fonts = 0;
    db->db_format = 0;
    db->glyph_version = NULL;
}

/* ---------------------------------------------------------------------------
 * Find
 * ------------------------------------------------------------------------- */

/*
 * Linear search by id. Returns a pointer into db->fonts (owned by the db) or
 * NULL when no entry matches.
 */
const glyph_installed_font_t *glyph_db_find(const glyph_db_t *db, const char *id)
{
    if (db == NULL || id == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < db->n_fonts; i++) {
        if (db->fonts[i].id != NULL && strcmp(db->fonts[i].id, id) == 0) {
            return &db->fonts[i];
        }
    }
    return NULL;
}

/* ---------------------------------------------------------------------------
 * Upsert
 * ------------------------------------------------------------------------- */

/*
 * Insert or replace the entry for font->id.
 *
 * Ownership: on SUCCESS this call takes ownership of the caller's `files`
 * buffer — it is realloc'd to hold n_files+1 slots with a trailing NULL and
 * stored in the record. On any allocation FAILURE the caller retains full
 * ownership of `files` (the original pointer remains valid and untouched).
 *
 * Returns 0 on success, -1 on allocation failure.
 */
int glyph_db_upsert(glyph_db_t *db, const glyph_font_t *font,
                    char **files, size_t n_files)
{
    if (db == NULL || font == NULL || font->id == NULL) {
        errno = EINVAL;
        return -1;
    }

    /* Locate an existing slot for this id, if any. */
    bool is_new = true;
    size_t idx = 0;
    for (size_t i = 0; i < db->n_fonts; i++) {
        if (db->fonts[i].id != NULL && strcmp(db->fonts[i].id, font->id) == 0) {
            idx = i;
            is_new = false;
            break;
        }
    }

    /*
     * Stage allocations that can fail BEFORE taking ownership of `files`.
     * Order matters: grow the fonts array first (if needed), then dup the
     * scalar strings, and only then realloc the caller's files buffer.
     */

    /* (1) Grow the fonts array for a brand-new entry. We commit the larger
     *     pointer to db->fonts immediately and zero the new slot so that an
     *     early return on a later allocation failure leaves db->fonts in a
     *     safe, freeable state (n_fonts is bumped only on full success). */
    if (is_new) {
        size_t new_cap = db->n_fonts + 1u;
        glyph_installed_font_t *na = realloc(db->fonts, new_cap * sizeof(*na));
        if (na == NULL) {
            return -1; /* files untouched */
        }
        memset(&na[db->n_fonts], 0, sizeof(na[db->n_fonts]));
        db->fonts = na;
        idx = db->n_fonts;
    }

    /* (2) Duplicate the scalar fields from the catalog font. */
    char *new_id = glyph_strdup(font->id);
    if (new_id == NULL) {
        return -1; /* files untouched; db->fonts may be over-allocated + zeroed */
    }

    char *new_name = NULL;
    if (font->name != NULL) {
        new_name = glyph_strdup(font->name);
        if (new_name == NULL) {
            free(new_id);
            return -1;
        }
    }

    char *new_version = NULL;
    if (font->version != NULL) {
        new_version = glyph_strdup(font->version);
        if (new_version == NULL) {
            free(new_id);
            free(new_name);
            return -1;
        }
    }

    /* install_date may legitimately be NULL on timestamp failure — tolerate. */
    char *new_install_date = glyph_timestamp_iso8601();

    /* (3) Take ownership of the caller's files buffer, growing it by one slot
     *     for the NULL terminator. On realloc failure the original pointer is
     *     still valid and remains the caller's responsibility. */
    char **new_files = realloc(files, (n_files + 1u) * sizeof(*new_files));
    if (new_files == NULL) {
        free(new_id);
        free(new_name);
        free(new_version);
        free(new_install_date);
        return -1;
    }
    new_files[n_files] = NULL;

    /* (4) Commit. For an existing slot, release the old contents first. */
    if (!is_new) {
        free_font_contents(&db->fonts[idx]);
    }

    glyph_installed_font_t *slot = &db->fonts[idx];
    slot->id = new_id;
    slot->name = new_name;
    slot->version = new_version;
    slot->revision = font->revision;
    slot->install_date = new_install_date;
    slot->files = new_files;

    if (is_new) {
        db->n_fonts++;
    }
    return 0;
}

/* ---------------------------------------------------------------------------
 * Remove
 * ------------------------------------------------------------------------- */

/*
 * Remove the entry matching id. The on-disk files it references are NOT
 * deleted (the caller is responsible for that). Returns 0 on success, -1 if
 * no matching entry exists. The fonts array is shrunk best-effort: if the
 * shrink realloc fails the slightly-too-large buffer is retained (still
 * correct since n_fonts is decremented).
 */
int glyph_db_remove(glyph_db_t *db, const char *id)
{
    if (db == NULL || id == NULL) {
        errno = EINVAL;
        return -1;
    }

    size_t idx = db->n_fonts;
    for (size_t i = 0; i < db->n_fonts; i++) {
        if (db->fonts[i].id != NULL && strcmp(db->fonts[i].id, id) == 0) {
            idx = i;
            break;
        }
    }
    if (idx == db->n_fonts) {
        return -1; /* not found */
    }

    free_font_contents(&db->fonts[idx]);

    /* Shift the tail down to keep the array dense. */
    if (idx + 1u < db->n_fonts) {
        memmove(&db->fonts[idx], &db->fonts[idx + 1u],
                (db->n_fonts - idx - 1u) * sizeof(*db->fonts));
    }
    db->n_fonts--;

    /* Best-effort shrink. When the DB becomes empty, free the block outright
     * (realloc(ptr, 0) is implementation-defined, so avoid relying on it). */
    if (db->n_fonts == 0u) {
        free(db->fonts);
        db->fonts = NULL;
    } else {
        glyph_installed_font_t *na = realloc(db->fonts, db->n_fonts * sizeof(*na));
        if (na != NULL) {
            db->fonts = na;
        }
        /* else: keep the over-allocated buffer; logically still correct. */
    }
    return 0;
}
