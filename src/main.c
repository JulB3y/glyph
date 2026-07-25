/*
 * src/main.c — glyph CLI entry point.
 *
 * Wires every other module (util, download, verify, manifest, extract, db,
 * fc_cache) into the user-facing command set. Compiles under C99 with
 *   -Wall -Wextra -Werror -Wstrict-prototypes -Wmissing-prototypes
 *
 * Notes:
 *   - Feature-test macros are defined FIRST so every system header sees them.
 *   - getopt_long is reset (optind set) at the start of each per-subcommand
 *     parse so stale state from a sibling command cannot leak in. For commands
 *     with positionals we set optind=2 so the subcommand name (argv[1]) is
 *     skipped and argv[optind] is the first user positional.
 *   - Every catalog/db/temp allocation is freed on every return path.
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
#include "complete.h"
#include "download.h"
#include "verify.h"
#include "manifest.h"
#include "extract.h"
#include "db.h"
#include "fc_cache.h"

#include <cjson/cJSON.h>

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <getopt.h>
#include <libgen.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static bool g_no_cache = false;
static bool g_verbose = false;
static bool g_quiet = false;

/* ---------------------------------------------------------------------------
 * usage
 * ------------------------------------------------------------------------- */

static void usage(FILE *out)
{
    fprintf(out, "glyph %s -- font package manager\n\n", GLYPH_VERSION);
    fputs(
        "Usage: glyph <command> [options] [args]\n\n"
        "Commands:\n"
        "  index update              Download and verify the catalog\n"
        "  index status              Show cached catalog information\n"
        "  search <query>            Search the catalog by id / name / tag\n"
        "  list [--catalog]          List installed fonts (or catalog fonts)\n"
        "  info <id>                 Show details for a font\n"
        "  install <id>[@rev|==ver]  Install a font from the catalog\n"
        "  remove <id>               Remove an installed font\n"
        "  upgrade [--all] [<id>]    Upgrade one font (or all installed fonts)\n"
        "  completions <fish|install>\n"
        "                            Print or install shell completions\n\n"
        "Common options:\n"
        "  --no-cache                Skip fontconfig cache refresh\n"
        "  -v, --verbose             Show full fc-cache output during refresh\n"
        "  -q, --quiet               Suppress download progress output\n"
        "  --debug                   Enable diagnostic tracing (or GLYPH_DEBUG=1)\n"
        "  -h, --help                Show this help\n"
        "  -v, --version             Show the glyph version\n\n"
        "Environment:\n"
        "  GLYPH_DEBUG               Set to a non-empty value other than 0 to\n"
        "                            enable diagnostic tracing on stderr\n",
        out);
}

/* ---------------------------------------------------------------------------
 * Helpers
 * ------------------------------------------------------------------------- */

/* Load the cached catalog into *cat (caller frees with glyph_catalog_free).
 * On any failure, logs "run `glyph index update` first" and returns
 * GLYPH_EXIT_NOT_FOUND. *cat is zeroed on failure. */
static int ensure_catalog(glyph_catalog_t *cat)
{
    memset(cat, 0, sizeof(*cat));
    char *path = glyph_path_catalog_cache();
    if (path == NULL) {
        glyph_log_err("run `glyph index update` first");
        return GLYPH_EXIT_NOT_FOUND;
    }
    int rc = glyph_catalog_load_file(path, cat);
    free(path);
    if (rc != 0) {
        glyph_log_err("run `glyph index update` first");
        return GLYPH_EXIT_NOT_FOUND;
    }
    return 0;
}

/* Recursive remove (rm -rf). Returns 0 on full success, -1 if any error. */
static int rmrf(const char *path)
{
    if (path == NULL) {
        return -1;
    }

    struct stat st;
    if (lstat(path, &st) != 0) {
        return -1;
    }
    if (!S_ISDIR(st.st_mode)) {
        return (unlink(path) == 0) ? 0 : -1;
    }

    DIR *d = opendir(path);
    if (d == NULL) {
        return -1;
    }

    int rc = 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) {
            continue;
        }
        char *child = glyph_path_join(path, de->d_name);
        if (child == NULL) {
            rc = -1;
            continue;
        }
        if (rmrf(child) != 0) {
            rc = -1;
        }
        free(child);
    }
    closedir(d);

    if (rmdir(path) != 0) {
        rc = -1;
    }
    return rc;
}

/* Case-insensitive substring test. Returns 1 if needle is found in hay (or
 * needle is NULL/empty), 0 otherwise. */
static int ci_substr(const char *hay, const char *needle)
{
    if (needle == NULL || needle[0] == '\0') {
        return 1;
    }
    if (hay == NULL) {
        return 0;
    }
    size_t nl = strlen(needle);
    size_t hl = strlen(hay);
    if (nl > hl) {
        return 0;
    }
    for (size_t i = 0; i + nl <= hl; i++) {
        size_t j = 0;
        for (; j < nl; j++) {
            if (tolower((unsigned char)hay[i + j]) !=
                tolower((unsigned char)needle[j])) {
                break;
            }
        }
        if (j == nl) {
            return 1;
        }
    }
    return 0;
}

/*
 * `index update` core: discover the latest release of julbey/glyph-catalog
 * via the Forgejo API, resolve the two required assets by exact name,
 * download them, verify the signature STRICTLY against the build-time
 * embedded key, and atomically cache catalog + signature + release tag.
 *
 * Runs under the data-dir lock. Returns a GLYPH_EXIT_* code (D6 mapping):
 *   3 network    - API unreachable / asset download failed
 *   4 integrity  - malformed API JSON, missing/non-HTTPS asset, bad sig/fp
 *   5 not-found  - API 404 (no release published)
 *   1 error      - cache write failure
 */
static int index_update(void)
{
    int ret = GLYPH_EXIT_ERROR;
    char *meta = NULL;
    size_t meta_len = 0;
    long http_status = 0;
    cJSON *root = NULL;
    char *cat_url = NULL;
    char *sig_url = NULL;
    char *cat_bytes = NULL;
    size_t cat_len = 0;
    char *sig_bytes = NULL;
    size_t sig_len = 0;
    glyph_catalog_t catobj;
    char *cpath = NULL;
    char *spath = NULL;
    char *dup = NULL;

    memset(&catobj, 0, sizeof(catobj));

    /* ---- release discovery ---- */
    if (glyph_download_memory_status(GLYPH_CATALOG_RELEASES_API, &meta,
                                     &meta_len, &http_status, NULL) != 0) {
        if (http_status == 404) {
            glyph_log_err("no catalog release published (%s returned 404)",
                          GLYPH_CATALOG_RELEASES_API);
            ret = GLYPH_EXIT_NOT_FOUND;
        } else {
            glyph_log_err("catalog release lookup failed");
            ret = GLYPH_EXIT_NETWORK;
        }
        goto out;
    }

    /* glyph_download_memory_status guarantees a NUL-terminated buffer. */
    root = cJSON_Parse(meta);
    if (root == NULL) {
        glyph_log_err("malformed release metadata (not JSON)");
        ret = GLYPH_EXIT_INTEGRITY;
        goto out;
    }

    const cJSON *jtag = cJSON_GetObjectItem(root, "tag_name");
    const cJSON *assets = cJSON_GetObjectItem(root, "assets");
    if (!cJSON_IsString(jtag) || jtag->valuestring == NULL ||
        jtag->valuestring[0] == '\0' || !cJSON_IsArray(assets)) {
        glyph_log_err("malformed release metadata (missing tag_name/assets)");
        ret = GLYPH_EXIT_INTEGRITY;
        goto out;
    }
    const char *tag = jtag->valuestring;

    /* ---- asset resolution: exact names, two required, rest ignored ---- */
    const cJSON *a;
    cJSON_ArrayForEach(a, assets) {
        const cJSON *name = cJSON_GetObjectItem(a, "name");
        const cJSON *url = cJSON_GetObjectItem(a, "browser_download_url");
        if (!cJSON_IsString(name) || name->valuestring == NULL ||
            !cJSON_IsString(url) || url->valuestring == NULL) {
            continue;
        }
        if (cat_url == NULL &&
            strcmp(name->valuestring, "catalog.json") == 0) {
            cat_url = glyph_strdup(url->valuestring);
        } else if (sig_url == NULL &&
                   strcmp(name->valuestring, "catalog.json.sig2") == 0) {
            sig_url = glyph_strdup(url->valuestring);
        }
        /* Any other asset (e.g. glyph-catalog.pub.pem) is ignored. */
    }

    if (cat_url == NULL || sig_url == NULL) {
        glyph_log_err("release %s is missing required asset: %s", tag,
                      (cat_url == NULL) ? "catalog.json"
                                        : "catalog.json.sig2");
        ret = GLYPH_EXIT_INTEGRITY;
        goto out;
    }
    if (!glyph_url_is_allowed(cat_url) || !glyph_url_is_allowed(sig_url)) {
        glyph_log_err("refusing non-HTTPS asset URL in release %s", tag);
        ret = GLYPH_EXIT_INTEGRITY;
        goto out;
    }

    /* ---- freshness: unchanged tag + complete cache -> skip downloads ---- */
    if (glyph_catalog_cache_is_current(tag)) {
        glyph_log_info("catalog already at %s", tag);
        ret = GLYPH_EXIT_OK;
        goto out;
    }

    /* ---- download both assets ---- */
    if (!g_quiet) {
        fprintf(stderr, "  updating catalog\n");
    }
    glyph_dl_opts_t cat_opts = { .label = "catalog", .quiet = g_quiet,
                                 .progress = true };
    if (glyph_download_memory(cat_url, &cat_bytes, &cat_len, &cat_opts) != 0) {
        glyph_log_err("catalog download failed");
        ret = GLYPH_EXIT_NETWORK;
        goto out;
    }
    if (glyph_download_memory(sig_url, &sig_bytes, &sig_len, NULL) != 0) {
        glyph_log_err("signature download failed");
        ret = GLYPH_EXIT_NETWORK;
        goto out;
    }

    /* ---- parse + strict verification against the embedded key ---- */
    double t_parse = glyph_now_sec();
    if (glyph_catalog_parse(cat_bytes, &catobj) != 0) {
        glyph_log_err("catalog parse error");
        ret = GLYPH_EXIT_INTEGRITY;
        goto out;
    }
    glyph_log_debug("catalog parse: ok (%zu fonts) elapsed=%.3fs",
                    catobj.n_fonts, glyph_now_sec() - t_parse);
    if (glyph_verify_catalog_signature(cat_bytes, cat_len,
                                       (const uint8_t *)sig_bytes, sig_len,
                                       catobj.signature_fingerprint) != 0) {
        /* glyph_verify_catalog_signature logged the precise diagnostic. */
        ret = GLYPH_EXIT_INTEGRITY;
        goto out;
    }

    /* ---- atomic cache write: catalog + signature + release tag ---- */
    double t_cache = glyph_now_sec();
    cpath = glyph_path_catalog_cache();
    spath = glyph_path_catalog_sig_cache();
    if (cpath == NULL || spath == NULL) {
        glyph_log_err("cannot resolve cache paths");
        ret = GLYPH_EXIT_ERROR;
        goto out;
    }
    /* The cache dir may not exist yet on a fresh install. */
    dup = glyph_strdup(cpath);
    if (dup != NULL) {
        (void)glyph_mkdir_p(dirname(dup), 0755);
        free(dup);
        dup = NULL;
    }
    if (glyph_write_file(cpath, cat_bytes, cat_len, 0644) != 0 ||
        glyph_write_file(spath, sig_bytes, sig_len, 0644) != 0 ||
        glyph_write_release_tag(tag) != 0) {
        glyph_log_err("could not write catalog cache");
        ret = GLYPH_EXIT_ERROR;
        goto out;
    }
    glyph_log_debug("cache write: ok (catalog+sig+tag %s) elapsed=%.3fs",
                    tag, glyph_now_sec() - t_cache);

    if (!g_quiet) {
        fprintf(stderr, "  catalog updated to %s\n", tag);
    }
    ret = GLYPH_EXIT_OK;

out:
    free(dup);
    free(cpath);
    free(spath);
    glyph_catalog_free(&catobj);
    free(cat_bytes);
    free(sig_bytes);
    free(cat_url);
    free(sig_url);
    cJSON_Delete(root);
    free(meta);
    return ret;
}

/*
 * Install/upgrade core. Shared by cmd_install and cmd_upgrade.
 *
 * Caller owns *db and *cat and releases them after this returns. On success,
 * the db entry for `id` has been updated and saved (caller persists db). The
 * caller is responsible for fc-cache refresh after releasing the lock.
 *
 * Returns a GLYPH_EXIT_* code.
 */
static int do_install(glyph_db_t *db, const glyph_catalog_t *cat,
                      const char *id, const char *pin_ver, int pin_rev)
{
    const glyph_font_t *f = glyph_catalog_find(cat, id);
    if (f == NULL) {
        glyph_log_err("font not found in catalog: %s", id);
        return GLYPH_EXIT_NOT_FOUND;
    }

    /* v1 simplification: catalog tracks one version per id, so version pins
     * are advisory only. Warn on mismatch but keep going with the catalog
     * entry. Revision pins are accepted but not enforced. */
    if (pin_ver != NULL && f->version != NULL &&
        strcmp(pin_ver, f->version) != 0) {
        glyph_log_warn("requested version %s unavailable; using catalog %s",
                       pin_ver, f->version);
    }
    (void)pin_rev;

    /* Already installed at the same version+revision? Nothing to do. */
    const glyph_installed_font_t *existing = glyph_db_find(db, id);
    if (existing != NULL && existing->version != NULL && f->version != NULL &&
        strcmp(existing->version, f->version) == 0 &&
        existing->revision == f->revision) {
        return GLYPH_EXIT_ALREADY_INSTALLED;
    }

    /* ---- files source: download individual files, verify, move ---- */
    if (f->source.type == GLYPH_SOURCE_FILES) {
        /* Validate names and URLs up front. */
        for (size_t i = 0; i < f->source.n_files; i++) {
            const glyph_source_file_t *sf = &f->source.files[i];
            if (sf->name == NULL || !glyph_path_is_safe(sf->name) ||
                strchr(sf->name, '/') != NULL) {
                glyph_log_err("unsafe file name in source: %s",
                              sf->name ? sf->name : "(null)");
                return GLYPH_EXIT_USAGE;
            }
            if (!glyph_url_is_allowed(sf->url)) {
                glyph_log_err("refusing non-HTTPS URL: %s",
                              sf->url ? sf->url : "(null)");
                return GLYPH_EXIT_USAGE;
            }
        }

        char *tmp = glyph_mkdtemp();
        if (tmp == NULL) {
            glyph_log_err("could not create temp dir: %s", strerror(errno));
            return GLYPH_EXIT_ERROR;
        }

        /* Download + verify each file into tmp. */
        for (size_t i = 0; i < f->source.n_files; i++) {
            const glyph_source_file_t *sf = &f->source.files[i];
            char *dst = glyph_path_join(tmp, sf->name);
            if (dst == NULL) {
                rmrf(tmp); free(tmp);
                return GLYPH_EXIT_ERROR;
            }
            char dl_label[256];
            snprintf(dl_label, sizeof(dl_label), "%s %s (%zu/%zu: %s)",
                     id, f->version ? f->version : "?",
                     i + 1, f->source.n_files, sf->name);
            glyph_dl_opts_t dl_opts = { .label = dl_label, .quiet = g_quiet,
                                        .progress = true };
            if (glyph_download_file(sf->url, dst, false, &dl_opts) != 0) {
                glyph_log_err("download failed: %s", sf->url);
                free(dst);
                rmrf(tmp); free(tmp);
                return GLYPH_EXIT_NETWORK;
            }
            if (sf->sha256 != NULL &&
                !glyph_sha256_verify_file(dst, sf->sha256)) {
                glyph_log_err("integrity check failed for %s (%s)",
                              id, sf->name);
                free(dst);
                rmrf(tmp); free(tmp);
                return GLYPH_EXIT_INTEGRITY;
            }
            free(dst);
        }

        char *destdir = glyph_path_font_dir(id, f->version);
        if (destdir == NULL || glyph_mkdir_p(destdir, 0755) != 0) {
            glyph_log_err("could not create destination directory");
            rmrf(tmp); free(destdir); free(tmp);
            return GLYPH_EXIT_ERROR;
        }

        /* Move verified files into destdir; build absolute-path array for db. */
        size_t nf = f->source.n_files;
        char **files = calloc(nf + 1, sizeof(*files));
        if (files == NULL) {
            rmrf(tmp); free(destdir); free(tmp);
            return GLYPH_EXIT_ERROR;
        }
        int mv_rc = 0;
        for (size_t i = 0; i < nf; i++) {
            const char *name = f->source.files[i].name;
            char *src_path = glyph_path_join(tmp, name);
            char *dst_path = glyph_path_join(destdir, name);
            if (src_path == NULL || dst_path == NULL) {
                free(src_path); free(dst_path);
                mv_rc = -1;
                break;
            }
            if (rename(src_path, dst_path) != 0) {
                if (errno == EXDEV) {
                    char *buf = NULL;
                    size_t blen = 0;
                    if (glyph_read_file(src_path, &buf, &blen) != 0 ||
                        glyph_write_file(dst_path, buf, blen, 0644) != 0) {
                        free(buf);
                        free(src_path); free(dst_path);
                        mv_rc = -1;
                        break;
                    }
                    free(buf);
                    unlink(src_path);
                } else {
                    free(src_path); free(dst_path);
                    mv_rc = -1;
                    break;
                }
            }
            files[i] = dst_path; /* ownership to db */
            free(src_path);
        }

        if (mv_rc != 0) {
            for (size_t i = 0; i < nf; i++) {
                free(files[i]);
            }
            free(files);
            rmrf(destdir);
            rmrf(tmp); free(destdir); free(tmp);
            glyph_log_err("failed to install files for %s", id);
            return GLYPH_EXIT_ERROR;
        }
        files[nf] = NULL;

        glyph_db_upsert(db, f, files, nf);

        rmrf(tmp); free(destdir); free(tmp);
        return GLYPH_EXIT_OK;
    }

    /* ---- archive source (default) ---- */
    if (!glyph_url_is_allowed(f->source.url)) {
        glyph_log_err("refusing non-HTTPS URL: %s",
                      f->source.url ? f->source.url : "(null)");
        return GLYPH_EXIT_USAGE;
    }

    char *tmp = glyph_mkdtemp();
    if (tmp == NULL) {
        glyph_log_err("could not create temp dir: %s", strerror(errno));
        return GLYPH_EXIT_ERROR;
    }

    const char *fmt = (f->source.format != NULL) ? f->source.format : "zip";
    char fname[256];
    snprintf(fname, sizeof(fname), "%s.%s", id, fmt);
    char *archive = glyph_path_join(tmp, fname);
    if (archive == NULL) {
        rmrf(tmp);
        free(tmp);
        return GLYPH_EXIT_ERROR;
    }

    char dl_label[256];
    snprintf(dl_label, sizeof(dl_label), "%s %s",
             id, f->version ? f->version : "?");
    glyph_dl_opts_t dl_opts = { .label = dl_label, .quiet = g_quiet,
                                .progress = true };
    if (glyph_download_file(f->source.url, archive, false, &dl_opts) != 0) {
        glyph_log_err("download failed: %s", f->source.url);
        rmrf(tmp);
        free(archive);
        free(tmp);
        return GLYPH_EXIT_NETWORK;
    }

    if (f->source.sha256 != NULL &&
        !glyph_sha256_verify_file(archive, f->source.sha256)) {
        glyph_log_err("integrity check failed for %s", id);
        rmrf(tmp);
        free(archive);
        free(tmp);
        return GLYPH_EXIT_INTEGRITY;
    }

    char *destdir = glyph_path_font_dir(id, f->version);
    if (destdir == NULL || glyph_mkdir_p(destdir, 0755) != 0) {
        glyph_log_err("could not create destination directory");
        rmrf(tmp);
        free(destdir);
        free(archive);
        free(tmp);
        return GLYPH_EXIT_ERROR;
    }

    char **files = NULL;
    size_t nfiles = 0;
    if (glyph_extract_zip(archive, destdir, &f->install, &files, &nfiles) != 0) {
        glyph_log_err("extraction failed for %s", id);
        rmrf(tmp);
        free(destdir);
        free(archive);
        free(tmp);
        return GLYPH_EXIT_ERROR;
    }

    /* db_upsert takes ownership of files[] -- do not free it here. */
    glyph_db_upsert(db, f, files, nfiles);

    rmrf(tmp);
    free(destdir);
    free(archive);
    free(tmp);
    return GLYPH_EXIT_OK;
}

/* ---------------------------------------------------------------------------
 * Subcommand: index
 * ------------------------------------------------------------------------- */

static int cmd_index(int argc, char **argv)
{
    if (argc < 3) {
        usage(stderr);
        return GLYPH_EXIT_USAGE;
    }
    const char *action = argv[2];
    if (strcmp(action, "update") != 0 && strcmp(action, "status") != 0) {
        usage(stderr);
        return GLYPH_EXIT_USAGE;
    }

    /* Best-effort flag scan after the action token. */
    for (int i = 3; i < argc; i++) {
        if (strcmp(argv[i], "--no-cache") == 0) {
            g_no_cache = true;
        } else if (strcmp(argv[i], "-q") == 0 ||
                   strcmp(argv[i], "--quiet") == 0) {
            g_quiet = true;
        } else if (strcmp(argv[i], "-h") == 0 ||
                   strcmp(argv[i], "--help") == 0) {
            usage(stdout);
            return GLYPH_EXIT_OK;
        }
    }

    /* ---- index status ---- */
    if (strcmp(action, "status") == 0) {
        char *cpath = glyph_path_catalog_cache();
        if (cpath == NULL) {
            glyph_log_err("cannot resolve cache directory");
            return GLYPH_EXIT_ERROR;
        }
        glyph_catalog_t cat;
        memset(&cat, 0, sizeof(cat));
        if (glyph_catalog_load_file(cpath, &cat) != 0) {
            printf("no catalog cached\n");
            free(cpath);
            return GLYPH_EXIT_OK;
        }
        char *rtag = glyph_read_release_tag();
        printf("release_tag:           %s\n", rtag ? rtag : "(unknown)");
        free(rtag);
        printf("version:               %s\n",
               cat.version ? cat.version : "(unknown)");
        printf("last_updated:          %s\n",
               cat.last_updated ? cat.last_updated : "(unknown)");
        printf("count:                 %d\n", cat.count);
        printf("signature_fingerprint: %s\n",
               cat.signature_fingerprint ? cat.signature_fingerprint
                                         : "(none)");
        char hash[65];
        if (glyph_sha256_file(cpath, hash) == 0) {
            printf("sha256:                %s\n", hash);
        }
        glyph_catalog_free(&cat);
        free(cpath);
        return GLYPH_EXIT_OK;
    }

    /* ---- index update ---- */
    glyph_lock_t *lk = NULL;
    if (glyph_lock_acquire(&lk) != 0) {
        if (errno == EWOULDBLOCK || errno == EAGAIN) {
            glyph_log_err("another glyph instance is running");
            return GLYPH_EXIT_LOCK;
        }
        glyph_log_err("failed to acquire lock: %s", strerror(errno));
        return GLYPH_EXIT_ERROR;
    }

    int rc = index_update();
    glyph_lock_release(lk);
    return rc;
}

/* ---------------------------------------------------------------------------
 * Subcommand: search
 * ------------------------------------------------------------------------- */

static int cmd_search(int argc, char **argv)
{
    static const struct option lopts[] = {
        {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0}
    };
    optind = 2;
    for (;;) {
        int idx = 0;
        int c = getopt_long(argc, argv, "h", lopts, &idx);
        if (c == -1) {
            break;
        }
        switch (c) {
        case 'h':
            usage(stdout);
            return GLYPH_EXIT_OK;
        default:
            usage(stderr);
            return GLYPH_EXIT_USAGE;
        }
    }

    const char *query = (optind < argc) ? argv[optind] : NULL;
    if (query == NULL) {
        usage(stderr);
        return GLYPH_EXIT_USAGE;
    }

    glyph_catalog_t cat;
    int rc = ensure_catalog(&cat);
    if (rc != 0) {
        return rc;
    }

    for (size_t i = 0; i < cat.n_fonts; i++) {
        const glyph_font_t *f = &cat.fonts[i];
        int match = ci_substr(f->id, query) || ci_substr(f->name, query);
        if (!match && f->tags != NULL) {
            for (size_t t = 0; f->tags[t] != NULL; t++) {
                if (ci_substr(f->tags[t], query)) {
                    match = 1;
                    break;
                }
            }
        }
        if (match) {
            printf("%-20s %s %s\n",
                   f->id ? f->id : "",
                   f->name ? f->name : "",
                   f->version ? f->version : "");
        }
    }

    glyph_catalog_free(&cat);
    return GLYPH_EXIT_OK;
}

/* ---------------------------------------------------------------------------
 * Subcommand: list
 * ------------------------------------------------------------------------- */

static int cmd_list(int argc, char **argv)
{
    bool catalog = false;
    static const struct option lopts[] = {
        {"catalog", no_argument, 0, 'c'},
        {"help",    no_argument, 0, 'h'},
        {0, 0, 0, 0}
    };
    optind = 2;
    for (;;) {
        int idx = 0;
        int c = getopt_long(argc, argv, "ch", lopts, &idx);
        if (c == -1) {
            break;
        }
        switch (c) {
        case 'c':
            catalog = true;
            break;
        case 'h':
            usage(stdout);
            return GLYPH_EXIT_OK;
        default:
            usage(stderr);
            return GLYPH_EXIT_USAGE;
        }
    }

    if (catalog) {
        glyph_catalog_t cat;
        int rc = ensure_catalog(&cat);
        if (rc != 0) {
            return rc;
        }
        for (size_t i = 0; i < cat.n_fonts; i++) {
            const glyph_font_t *f = &cat.fonts[i];
            printf("%-20s %-30s %s\n",
                   f->id ? f->id : "",
                   f->name ? f->name : "",
                   f->version ? f->version : "");
        }
        glyph_catalog_free(&cat);
        return GLYPH_EXIT_OK;
    }

    glyph_db_t db;
    memset(&db, 0, sizeof(db));
    if (glyph_db_load(&db) != 0) {
        /* Treat a missing/corrupt DB as "nothing installed". */
        printf("no fonts installed\n");
        return GLYPH_EXIT_OK;
    }
    if (db.n_fonts == 0) {
        printf("no fonts installed\n");
        glyph_db_free(&db);
        return GLYPH_EXIT_OK;
    }
    for (size_t i = 0; i < db.n_fonts; i++) {
        const glyph_installed_font_t *e = &db.fonts[i];
        printf("%-20s %-20s rev=%d  (%s)\n",
               e->id ? e->id : "",
               e->version ? e->version : "",
               e->revision,
               e->install_date ? e->install_date : "");
    }
    glyph_db_free(&db);
    return GLYPH_EXIT_OK;
}

/* ---------------------------------------------------------------------------
 * Subcommand: info
 * ------------------------------------------------------------------------- */

static int cmd_info(int argc, char **argv)
{
    static const struct option lopts[] = {
        {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0}
    };
    optind = 2;
    for (;;) {
        int idx = 0;
        int c = getopt_long(argc, argv, "h", lopts, &idx);
        if (c == -1) {
            break;
        }
        switch (c) {
        case 'h':
            usage(stdout);
            return GLYPH_EXIT_OK;
        default:
            usage(stderr);
            return GLYPH_EXIT_USAGE;
        }
    }

    const char *id = (optind < argc) ? argv[optind] : NULL;
    if (id == NULL) {
        usage(stderr);
        return GLYPH_EXIT_USAGE;
    }

    /* Prefer the installed record if present. */
    glyph_db_t db;
    memset(&db, 0, sizeof(db));
    if (glyph_db_load(&db) == 0) {
        const glyph_installed_font_t *inst = glyph_db_find(&db, id);
        if (inst != NULL) {
            printf("id:           %s\n", inst->id ? inst->id : "");
            printf("name:         %s\n", inst->name ? inst->name : "");
            printf("version:      %s\n",
                   inst->version ? inst->version : "");
            printf("revision:     %d\n", inst->revision);
            printf("install_date: %s\n",
                   inst->install_date ? inst->install_date : "");
            if (inst->files != NULL) {
                printf("files:\n");
                for (size_t i = 0; inst->files[i] != NULL; i++) {
                    printf("  %s\n", inst->files[i]);
                }
            }
            glyph_db_free(&db);
            return GLYPH_EXIT_OK;
        }
        glyph_db_free(&db);
    }

    /* Otherwise fall back to the catalog entry. */
    glyph_catalog_t cat;
    int rc = ensure_catalog(&cat);
    if (rc != 0) {
        return rc;
    }
    const glyph_font_t *f = glyph_catalog_find(&cat, id);
    if (f == NULL) {
        glyph_log_err("not found: %s", id);
        glyph_catalog_free(&cat);
        return GLYPH_EXIT_NOT_FOUND;
    }

    printf("id:           %s\n", f->id ? f->id : "");
    printf("name:         %s\n", f->name ? f->name : "");
    printf("author:       %s\n", f->author ? f->author : "");
    printf("license:      %s\n", f->license ? f->license : "");
    printf("category:     %s\n", f->category ? f->category : "");
    printf("description:  %s\n", f->description ? f->description : "");
    printf("version:      %s\n", f->version ? f->version : "");
    printf("revision:     %d\n", f->revision);
    printf("homepage:     %s\n", f->homepage ? f->homepage : "");
    if (f->tags != NULL) {
        printf("tags:        ");
        for (size_t i = 0; f->tags[i] != NULL; i++) {
            printf("%s%s", (i == 0) ? "" : ", ", f->tags[i]);
        }
        printf("\n");
    }
    printf("source.url:   %s\n",
           f->source.url ? f->source.url : "");
    printf("source.sha256:%s\n",
           f->source.sha256 ? f->source.sha256 : "");

    glyph_catalog_free(&cat);
    return GLYPH_EXIT_OK;
}

/* ---------------------------------------------------------------------------
 * Subcommand: install
 * ------------------------------------------------------------------------- */

static int cmd_install(int argc, char **argv)
{
    static const struct option lopts[] = {
        {"no-cache", no_argument, 0, 'N'},
        {"verbose",  no_argument, 0, 'v'},
        {"quiet",    no_argument, 0, 'q'},
        {"help",     no_argument, 0, 'h'},
        {0, 0, 0, 0}
    };
    optind = 2;
    for (;;) {
        int idx = 0;
        int c = getopt_long(argc, argv, "Nvqh", lopts, &idx);
        if (c == -1) {
            break;
        }
        switch (c) {
        case 'N':
            g_no_cache = true;
            break;
        case 'v':
            g_verbose = true;
            break;
        case 'q':
            g_quiet = true;
            break;
        case 'h':
            usage(stdout);
            return GLYPH_EXIT_OK;
        default:
            usage(stderr);
            return GLYPH_EXIT_USAGE;
        }
    }

    const char *spec = (optind < argc) ? argv[optind] : NULL;
    if (spec == NULL) {
        usage(stderr);
        return GLYPH_EXIT_USAGE;
    }

    /* Parse spec: "id", "id==ver", "id@rev", "id==ver@rev", "id@rev==ver".
     * id points into spec_c and stays valid through the call. */
    char *spec_c = glyph_strdup(spec);
    if (spec_c == NULL) {
        glyph_log_err("out of memory");
        return GLYPH_EXIT_ERROR;
    }
    char *id = spec_c;
    const char *pin_ver = NULL;
    int pin_rev = 0;

    char *at = strchr(spec_c, '@');
    char *eq = strstr(spec_c, "==");
    char *first;
    size_t first_skip;
    if (at != NULL && (eq == NULL || at < eq)) {
        first = at;
        first_skip = 1;
    } else if (eq != NULL) {
        first = eq;
        first_skip = 2;
    } else {
        first = NULL;
        first_skip = 0;
    }

    if (first != NULL) {
        *first = '\0';
        char *rest = first + first_skip;
        if (first == at) {
            char *eq2 = strstr(rest, "==");
            if (eq2 != NULL) {
                *eq2 = '\0';
                pin_rev = atoi(rest);
                pin_ver = eq2 + 2;
            } else {
                pin_rev = atoi(rest);
            }
        } else {
            char *at2 = strchr(rest, '@');
            if (at2 != NULL) {
                *at2 = '\0';
                pin_ver = rest;
                pin_rev = atoi(at2 + 1);
            } else {
                pin_ver = rest;
            }
        }
    }

    if (id[0] == '\0') {
        glyph_log_err("empty id in spec: %s", spec);
        free(spec_c);
        return GLYPH_EXIT_USAGE;
    }

    glyph_lock_t *lk = NULL;
    if (glyph_lock_acquire(&lk) != 0) {
        int code = (errno == EWOULDBLOCK || errno == EAGAIN)
                       ? GLYPH_EXIT_LOCK
                       : GLYPH_EXIT_ERROR;
        if (code == GLYPH_EXIT_LOCK) {
            glyph_log_err("another glyph instance is running");
        } else {
            glyph_log_err("failed to acquire lock: %s", strerror(errno));
        }
        free(spec_c);
        return code;
    }

    glyph_catalog_t cat;
    int rc = ensure_catalog(&cat);
    if (rc != 0) {
        glyph_lock_release(lk);
        free(spec_c);
        return rc;
    }

    glyph_db_t db;
    memset(&db, 0, sizeof(db));
    /* Missing DB is fine on first install. */
    (void)glyph_db_load(&db);

    /* Defer the success line until after the (slow) fc-cache refresh so it
     * is the final stdout output. id aliases spec_c (freed last); the catalog
     * version is duplicated since the catalog is freed below. */
    char *out_ver = NULL;
    rc = do_install(&db, &cat, id, pin_ver, pin_rev);
    if (rc == GLYPH_EXIT_OK) {
        if (glyph_db_save(&db) != 0) {
            glyph_log_warn("could not persist install database");
        }
        const glyph_font_t *f = glyph_catalog_find(&cat, id);
        if (f != NULL && f->version != NULL) {
            out_ver = glyph_strdup(f->version);
        }
    }

    glyph_db_free(&db);
    glyph_catalog_free(&cat);
    glyph_lock_release(lk);

    if (rc == GLYPH_EXIT_OK && !g_no_cache) {
        if (fc_cache_refresh(false, g_verbose) != 0) {
            glyph_log_warn("fc-cache refresh failed (continuing)");
        }
    }

    if (rc == GLYPH_EXIT_OK) {
        if (out_ver != NULL) {
            printf("installed %s %s\n", id, out_ver);
        } else {
            printf("installed %s\n", id);
        }
    } else if (rc == GLYPH_EXIT_ALREADY_INSTALLED) {
        printf("%s already installed\n", id);
    }
    fflush(stdout);
    free(out_ver);
    free(spec_c);
    return rc;
}

/* ---------------------------------------------------------------------------
 * Subcommand: remove
 * ------------------------------------------------------------------------- */

static int cmd_remove(int argc, char **argv)
{
    static const struct option lopts[] = {
        {"no-cache", no_argument, 0, 'N'},
        {"verbose",  no_argument, 0, 'v'},
        {"help",     no_argument, 0, 'h'},
        {0, 0, 0, 0}
    };
    optind = 2;
    for (;;) {
        int idx = 0;
        int c = getopt_long(argc, argv, "Nvh", lopts, &idx);
        if (c == -1) {
            break;
        }
        switch (c) {
        case 'N':
            g_no_cache = true;
            break;
        case 'v':
            g_verbose = true;
            break;
        case 'h':
            usage(stdout);
            return GLYPH_EXIT_OK;
        default:
            usage(stderr);
            return GLYPH_EXIT_USAGE;
        }
    }

    const char *id = (optind < argc) ? argv[optind] : NULL;
    if (id == NULL) {
        usage(stderr);
        return GLYPH_EXIT_USAGE;
    }

    glyph_lock_t *lk = NULL;
    if (glyph_lock_acquire(&lk) != 0) {
        int code = (errno == EWOULDBLOCK || errno == EAGAIN)
                       ? GLYPH_EXIT_LOCK
                       : GLYPH_EXIT_ERROR;
        if (code == GLYPH_EXIT_LOCK) {
            glyph_log_err("another glyph instance is running");
        } else {
            glyph_log_err("failed to acquire lock: %s", strerror(errno));
        }
        return code;
    }

    glyph_db_t db;
    memset(&db, 0, sizeof(db));
    if (glyph_db_load(&db) != 0) {
        glyph_log_err("not installed: %s", id);
        glyph_lock_release(lk);
        return GLYPH_EXIT_NOT_FOUND;
    }

    const glyph_installed_font_t *inst = glyph_db_find(&db, id);
    if (inst == NULL) {
        glyph_log_err("not installed: %s", id);
        glyph_db_free(&db);
        glyph_lock_release(lk);
        return GLYPH_EXIT_NOT_FOUND;
    }

    /* Unlink every recorded file (tolerate already-missing files). */
    if (inst->files != NULL) {
        for (size_t i = 0; inst->files[i] != NULL; i++) {
            if (unlink(inst->files[i]) != 0 && errno != ENOENT) {
                glyph_log_warn("could not unlink %s: %s",
                               inst->files[i], strerror(errno));
            }
        }
    }

    /* Best-effort cleanup of the now-empty version dir, then the id dir. */
    char *verdir = glyph_path_font_dir(inst->id, inst->version);
    if (verdir != NULL) {
        (void)rmdir(verdir);
        char *copy = glyph_strdup(verdir);
        if (copy != NULL) {
            char *iddir = dirname(copy);
            if (iddir != NULL) {
                (void)rmdir(iddir);
            }
            free(copy);
        }
        free(verdir);
    }

    glyph_db_remove(&db, id);
    if (glyph_db_save(&db) != 0) {
        glyph_log_warn("could not persist install database");
    }

    glyph_db_free(&db);
    glyph_lock_release(lk);

    if (!g_no_cache) {
        if (fc_cache_refresh(false, g_verbose) != 0) {
            glyph_log_warn("fc-cache refresh failed (continuing)");
        }
    }
    /* "removed" is the terminal stdout output, printed after fc-cache. */
    printf("removed %s\n", id);
    fflush(stdout);
    return GLYPH_EXIT_OK;
}

/* ---------------------------------------------------------------------------
 * Deferred result-line buffer (used by cmd_upgrade)
 * ------------------------------------------------------------------------- */

/* Append a printf-formatted line to a growable array of owned strings. Used
 * to collect stdout result lines during the work phase and emit them as the
 * final output after the fc-cache refresh. Allocation failures are logged
 * and the line is dropped (best-effort). */
static void push_line(char ***arr, size_t *n, size_t *cap,
                      const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int need = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (need < 0) {
        return;
    }

    char *line = malloc((size_t)need + 1);
    if (line == NULL) {
        glyph_log_warn("out of memory formatting result line");
        return;
    }
    va_start(ap, fmt);
    vsnprintf(line, (size_t)need + 1, fmt, ap);
    va_end(ap);

    if (*n == *cap) {
        size_t ncap = (*cap == 0) ? 8 : *cap * 2;
        char **narr = realloc(*arr, ncap * sizeof(*narr));
        if (narr == NULL) {
            glyph_log_warn("out of memory formatting result line");
            free(line);
            return;
        }
        *arr = narr;
        *cap = ncap;
    }
    (*arr)[(*n)++] = line;
}

/* ---------------------------------------------------------------------------
 * Subcommand: upgrade
 * ------------------------------------------------------------------------- */

static int cmd_upgrade(int argc, char **argv)
{
    bool all = false;
    static const struct option lopts[] = {
        {"all",      no_argument, 0, 'a'},
        {"no-cache", no_argument, 0, 'N'},
        {"verbose",  no_argument, 0, 'v'},
        {"quiet",    no_argument, 0, 'q'},
        {"help",     no_argument, 0, 'h'},
        {0, 0, 0, 0}
    };
    optind = 2;
    for (;;) {
        int idx = 0;
        int c = getopt_long(argc, argv, "aNvqh", lopts, &idx);
        if (c == -1) {
            break;
        }
        switch (c) {
        case 'a':
            all = true;
            break;
        case 'N':
            g_no_cache = true;
            break;
        case 'v':
            g_verbose = true;
            break;
        case 'q':
            g_quiet = true;
            break;
        case 'h':
            usage(stdout);
            return GLYPH_EXIT_OK;
        default:
            usage(stderr);
            return GLYPH_EXIT_USAGE;
        }
    }

    if (!all && optind >= argc) {
        usage(stderr);
        return GLYPH_EXIT_USAGE;
    }

    glyph_lock_t *lk = NULL;
    if (glyph_lock_acquire(&lk) != 0) {
        int code = (errno == EWOULDBLOCK || errno == EAGAIN)
                       ? GLYPH_EXIT_LOCK
                       : GLYPH_EXIT_ERROR;
        if (code == GLYPH_EXIT_LOCK) {
            glyph_log_err("another glyph instance is running");
        } else {
            glyph_log_err("failed to acquire lock: %s", strerror(errno));
        }
        return code;
    }

    glyph_db_t db;
    memset(&db, 0, sizeof(db));
    (void)glyph_db_load(&db);

    glyph_catalog_t cat;
    int rc = ensure_catalog(&cat);
    if (rc != 0) {
        glyph_db_free(&db);
        glyph_lock_release(lk);
        return rc;
    }

    int final_rc = GLYPH_EXIT_OK;
    bool changed = false;
    size_t n_upgraded = 0;

    /* Deferred stdout result lines: collected during the work phase and
     * emitted as the final output after the fc-cache refresh. Each entry is
     * an owned malloc'd string appended via push_line(). */
    char **lines = NULL;
    size_t n_lines = 0, cap_lines = 0;

    if (all) {
        /* Snapshot ids first: do_install may realloc db.fonts. */
        size_t n_ids = db.n_fonts;
        char **ids = NULL;
        if (n_ids > 0) {
            ids = calloc(n_ids, sizeof(*ids));
            if (ids == NULL) {
                glyph_log_err("out of memory");
                glyph_catalog_free(&cat);
                glyph_db_free(&db);
                glyph_lock_release(lk);
                return GLYPH_EXIT_ERROR;
            }
            for (size_t i = 0; i < n_ids; i++) {
                ids[i] = glyph_strdup(db.fonts[i].id ? db.fonts[i].id : "");
                if (ids[i] == NULL) {
                    for (size_t j = 0; j < i; j++) {
                        free(ids[j]);
                    }
                    free(ids);
                    glyph_log_err("out of memory");
                    glyph_catalog_free(&cat);
                    glyph_db_free(&db);
                    glyph_lock_release(lk);
                    return GLYPH_EXIT_ERROR;
                }
            }
        }

        for (size_t i = 0; i < n_ids; i++) {
            const char *this_id = ids[i];
            const glyph_font_t *f2 = glyph_catalog_find(&cat, this_id);
            if (f2 == NULL) {
                continue;
            }
            const glyph_installed_font_t *inst = glyph_db_find(&db, this_id);
            if (inst != NULL && inst->version != NULL && f2->version != NULL &&
                strcmp(inst->version, f2->version) == 0) {
                continue; /* already up to date */
            }
            char *old_ver = (inst != NULL && inst->version != NULL)
                                ? glyph_strdup(inst->version)
                                : NULL;
            int irc = do_install(&db, &cat, this_id, NULL, 0);
            if (irc == GLYPH_EXIT_OK) {
                changed = true;
                n_upgraded++;
                if (old_ver != NULL && f2->version != NULL) {
                    push_line(&lines, &n_lines, &cap_lines,
                              "upgraded %s %s -> %s\n", this_id, old_ver,
                              f2->version);
                } else {
                    push_line(&lines, &n_lines, &cap_lines, "upgraded %s\n",
                              this_id);
                }
            } else if (irc == GLYPH_EXIT_ALREADY_INSTALLED ||
                       irc == GLYPH_EXIT_NOT_FOUND) {
                /* skip */
            } else {
                final_rc = irc;
            }
            free(old_ver);
            if (final_rc != GLYPH_EXIT_OK) {
                break;
            }
        }

        for (size_t i = 0; i < n_ids; i++) {
            free(ids[i]);
        }
        free(ids);
    } else {
        const char *id = argv[optind];
        const glyph_font_t *f2 = glyph_catalog_find(&cat, id);
        if (f2 == NULL) {
            glyph_log_err("not in catalog: %s", id);
            glyph_catalog_free(&cat);
            glyph_db_free(&db);
            glyph_lock_release(lk);
            return GLYPH_EXIT_NOT_FOUND;
        }
        const glyph_installed_font_t *inst = glyph_db_find(&db, id);
        if (inst != NULL && inst->version != NULL && f2->version != NULL &&
            strcmp(inst->version, f2->version) == 0) {
            push_line(&lines, &n_lines, &cap_lines, "%s already up to date\n",
                      id);
        } else {
            char *old_ver = (inst != NULL && inst->version != NULL)
                                ? glyph_strdup(inst->version)
                                : NULL;
            int irc = do_install(&db, &cat, id, NULL, 0);
            if (irc == GLYPH_EXIT_OK) {
                changed = true;
                n_upgraded++;
                if (old_ver != NULL && f2->version != NULL) {
                    push_line(&lines, &n_lines, &cap_lines,
                              "upgraded %s %s -> %s\n", id, old_ver,
                              f2->version);
                } else if (inst == NULL && f2->version != NULL) {
                    push_line(&lines, &n_lines, &cap_lines,
                              "installed %s %s\n", id, f2->version);
                } else if (inst == NULL) {
                    push_line(&lines, &n_lines, &cap_lines, "installed %s\n",
                              id);
                } else {
                    push_line(&lines, &n_lines, &cap_lines, "upgraded %s\n",
                              id);
                }
            } else {
                final_rc = irc;
            }
            free(old_ver);
        }
    }

    if (all) {
        if (n_upgraded > 0) {
            push_line(&lines, &n_lines, &cap_lines, "upgraded %zu font%s\n",
                      n_upgraded, n_upgraded == 1 ? "" : "s");
        } else if (final_rc == GLYPH_EXIT_OK) {
            push_line(&lines, &n_lines, &cap_lines, "all fonts up to date\n");
        }
    }

    if (changed) {
        if (glyph_db_save(&db) != 0) {
            glyph_log_warn("could not persist install database");
        }
    }

    glyph_catalog_free(&cat);
    glyph_db_free(&db);
    glyph_lock_release(lk);

    if (changed && final_rc == GLYPH_EXIT_OK && !g_no_cache) {
        if (fc_cache_refresh(false, g_verbose) != 0) {
            glyph_log_warn("fc-cache refresh failed (continuing)");
        }
    }

    /* Result lines are the final stdout output, printed after the (slow)
     * fc-cache refresh so they signal true completion. */
    for (size_t i = 0; i < n_lines; i++) {
        fputs(lines[i], stdout);
    }
    fflush(stdout);
    for (size_t i = 0; i < n_lines; i++) {
        free(lines[i]);
    }
    free(lines);
    return final_rc;
}

/* ---------------------------------------------------------------------------
 * Subcommand: completions
 * ------------------------------------------------------------------------- */

static int cmd_completions(int argc, char **argv)
{
    static const struct option lopts[] = {
        {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0}
    };
    optind = 2;
    for (;;) {
        int idx = 0;
        int c = getopt_long(argc, argv, "h", lopts, &idx);
        if (c == -1) {
            break;
        }
        switch (c) {
        case 'h':
            usage(stdout);
            return GLYPH_EXIT_OK;
        default:
            usage(stderr);
            return GLYPH_EXIT_USAGE;
        }
    }

    const char *action = (optind < argc) ? argv[optind] : NULL;
    if (action == NULL) {
        usage(stderr);
        return GLYPH_EXIT_USAGE;
    }

    if (strcmp(action, "fish") == 0) {
        fputs(glyph_complete_fish_script(), stdout);
        return GLYPH_EXIT_OK;
    }

    if (strcmp(action, "install") == 0) {
        char *path = NULL;
        if (glyph_complete_install_fish(&path) != 0) {
            glyph_log_err("could not install fish completions: %s",
                          strerror(errno));
            return GLYPH_EXIT_ERROR;
        }
        glyph_log_info("installed fish completions to %s",
                       path ? path : "(unknown)");
        glyph_log_info("restart fish or run `exec fish` to enable");
        free(path);
        return GLYPH_EXIT_OK;
    }

    glyph_log_err("unsupported shell: %s (only fish is supported)", action);
    return GLYPH_EXIT_USAGE;
}

/* ---------------------------------------------------------------------------
 * Entry point
 * ------------------------------------------------------------------------- */

int main(int argc, char **argv)
{
    if (argc < 2) {
        usage(stderr);
        return GLYPH_EXIT_USAGE;
    }

    /* Global --debug flag: may appear before the subcommand. */
    bool debug_flag = false;
    if (strcmp(argv[1], "--debug") == 0) {
        debug_flag = true;
        argc--;
        argv++;
        if (argc < 2) {
            usage(stderr);
            return GLYPH_EXIT_USAGE;
        }
    }

    /* Resolve debug activation once: flag OR env (non-empty, not "0"). */
    const char *env_dbg = getenv("GLYPH_DEBUG");
    bool env_on = (env_dbg != NULL && env_dbg[0] != '\0' &&
                   strcmp(env_dbg, "0") != 0);
    if (debug_flag || env_on) {
        glyph_debug_set(true);
    }

    const char *cmd = argv[1];

    if (strcmp(cmd, "-h") == 0 || strcmp(cmd, "--help") == 0 ||
        strcmp(cmd, "help") == 0) {
        usage(stdout);
        return GLYPH_EXIT_OK;
    }
    if (strcmp(cmd, "-v") == 0 || strcmp(cmd, "--version") == 0) {
        puts(GLYPH_VERSION);
        return GLYPH_EXIT_OK;
    }

    if (strcmp(cmd, "index") == 0) {
        return cmd_index(argc, argv);
    }
    if (strcmp(cmd, "search") == 0) {
        return cmd_search(argc, argv);
    }
    if (strcmp(cmd, "list") == 0) {
        return cmd_list(argc, argv);
    }
    if (strcmp(cmd, "info") == 0) {
        return cmd_info(argc, argv);
    }
    if (strcmp(cmd, "install") == 0) {
        return cmd_install(argc, argv);
    }
    if (strcmp(cmd, "remove") == 0) {
        return cmd_remove(argc, argv);
    }
    if (strcmp(cmd, "upgrade") == 0) {
        return cmd_upgrade(argc, argv);
    }
    if (strcmp(cmd, "completions") == 0) {
        return cmd_completions(argc, argv);
    }
    if (strcmp(cmd, "__complete") == 0) {
        /* Hidden completion backend; deliberately excluded from usage(). */
        glyph_complete_emit((argc > 2) ? argv[2] : "",
                            (argc > 3) ? argv[3] : "");
        return GLYPH_EXIT_OK;
    }

    usage(stderr);
    return GLYPH_EXIT_USAGE;
}
