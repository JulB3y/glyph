/*
 * src/extract.c — ZIP extraction with install filters and traversal safety.
 *
 * Full implementation of the contract declared in include/extract.h. Wraps the
 * miniz ZIP reader and applies a glyph_install_t policy (include/exclude
 * globs, strip_components) while extracting into a destination directory.
 *
 * Designed to compile cleanly under:
 *   gcc -std=c99 -Wall -Wextra -Werror -Wstrict-prototypes -Wmissing-prototypes
 *
 * Notes:
 *   - Feature-test macros are defined FIRST so every system header sees them.
 *   - Path-traversal safety is enforced before any byte is written to disk:
 *     every in-archive entry name is vetted by glyph_path_is_safe(), which
 *     rejects absolute paths and any component equal to "..".
 *   - Include/exclude patterns are matched against the ORIGINAL in-archive
 *     path; only strip_components is applied afterwards.
 *   - On failure every path already collected plus the result array is freed
 *     and the miniz reader is closed; extracted on-disk files are left in
 *     place (the caller decides whether to roll them back).
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
#include "extract.h"
#include "util.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <miniz.h>

/* ---------------------------------------------------------------------------
 * Internal helpers
 * ------------------------------------------------------------------------- */

/*
 * Derive the parent directory of `path` by making a writable copy and
 * replacing the final '/' with '\0'. Returns the malloc'd buffer (now holding
 * the dirname) on success, or NULL if `path` has no usable parent (no '/',
 * or only a leading '/'), or on NULL input / OOM. Caller frees.
 */
static char *derive_dirname(const char *path)
{
    if (path == NULL) {
        return NULL;
    }
    size_t len = strlen(path);
    char *buf = malloc(len + 1);
    if (buf == NULL) {
        return NULL;
    }
    memcpy(buf, path, len + 1);

    char *slash = strrchr(buf, '/');
    if (slash == NULL || slash == buf) {
        free(buf);
        return NULL;
    }
    *slash = '\0';
    return buf;
}

/*
 * Strip `n` leading slash-separated components from `orig`. On success returns
 * a pointer into `orig` at the start of the remaining tail (never empty). If
 * stripping would consume the entire path (or `orig` has fewer than `n`
 * components), returns NULL so the caller can skip the entry.
 */
static const char *strip_leading_components(const char *orig, int n)
{
    const char *p = orig;
    int remaining = n;
    while (remaining > 0) {
        const char *slash = strchr(p, '/');
        if (slash == NULL) {
            return NULL; /* fewer than `n` components */
        }
        p = slash + 1;
        remaining--;
    }
    if (*p == '\0') {
        return NULL; /* stripping consumed the entire path */
    }
    return p;
}

/* ---------------------------------------------------------------------------
 * Glob matching
 * ------------------------------------------------------------------------- */

/*
 * Classic shell glob match supporting '*' (any run of chars) and '?'
 * (single char). Backtracking on '*' keeps the worst case tractable for the
 * small patterns used in font manifests. Matching is performed on the raw
 * string (no path-component special-casing), is case-sensitive, and a NUL
 * terminator in either argument ends matching correctly. NULL inputs never
 * match.
 */
bool glyph_glob_match(const char *pattern, const char *str)
{
    if (pattern == NULL || str == NULL) {
        return false;
    }

    const char *p = pattern;
    const char *s = str;
    const char *star = NULL;  /* pattern char just after the last '*' */
    const char *match = NULL; /* str position to resume from on backtrack */

    while (*s != '\0') {
        if (*p == *s || *p == '?') {
            p++;
            s++;
        } else if (*p == '*') {
            star = p + 1;
            match = s;
            p++; /* '*' matches zero chars for now */
        } else if (star != NULL) {
            /* Backtrack: let the previous '*' swallow one more char. */
            p = star;
            match++;
            s = match;
        } else {
            return false;
        }
    }

    /* A trailing run of '*' matches the empty tail of `str`. */
    while (*p == '*') {
        p++;
    }
    return *p == '\0';
}

/* ---------------------------------------------------------------------------
 * Path-traversal safety
 * ------------------------------------------------------------------------- */

/*
 * Reject paths that could escape the destination directory: NULL/empty
 * inputs, absolute paths (leading '/'), and any path containing a component
 * equal to "..". Components are split on '/'; a single "." is allowed. All
 * other inputs are considered safe (this is a lexical check — it does not
 * resolve symlinks, which is the caller's concern).
 */
bool glyph_path_is_safe(const char *path)
{
    if (path == NULL || path[0] == '\0') {
        return false;
    }
    if (path[0] == '/') {
        return false; /* absolute */
    }

    const char *p = path;
    while (*p != '\0') {
        const char *comp = p;
        while (*p != '\0' && *p != '/') {
            p++;
        }
        size_t complen = (size_t)(p - comp);
        if (complen == 2 && comp[0] == '.' && comp[1] == '.') {
            return false;
        }
        if (*p == '/') {
            p++;
        }
    }
    return true;
}

/* ---------------------------------------------------------------------------
 * ZIP extraction
 * ------------------------------------------------------------------------- */

int glyph_extract_zip(const char *archive_path, const char *dest_dir,
                      const glyph_install_t *install,
                      char ***out_files, size_t *out_count)
{
    if (archive_path == NULL || dest_dir == NULL ||
        out_files == NULL || out_count == NULL) {
        errno = EINVAL;
        return -1;
    }
    *out_files = NULL;
    *out_count = 0;

    if (glyph_mkdir_p(dest_dir, 0755) == -1) {
        return -1;
    }

    mz_zip_archive arch;
    mz_zip_zero_struct(&arch);
    if (!mz_zip_reader_init_file(&arch, archive_path, 0)) {
        return -1;
    }

    char **files = NULL;
    size_t count = 0;
    size_t cap = 0;
    bool ok = false; /* set true only on the full-success path */

    mz_uint n = mz_zip_reader_get_num_files(&arch);
    for (mz_uint i = 0; i < n; i++) {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&arch, i, &st)) {
            goto done;
        }
        if (st.m_is_directory) {
            continue; /* directories are created on demand below */
        }

        const char *orig = st.m_filename;

        /* (4) Path-traversal safety: vet the raw in-archive name. */
        if (!glyph_path_is_safe(orig)) {
            glyph_log_warn("skipping unsafe archive entry \"%s\"", orig);
            continue;
        }

        /* (5) include/exclude filtering, matched against `orig`. */
        if (install != NULL && install->include != NULL &&
            install->include[0] != NULL) {
            bool included = false;
            for (size_t k = 0; install->include[k] != NULL; k++) {
                if (glyph_glob_match(install->include[k], orig)) {
                    included = true;
                    break;
                }
            }
            if (!included) {
                continue;
            }
        }
        if (install != NULL && install->exclude != NULL) {
            bool excluded = false;
            for (size_t k = 0; install->exclude[k] != NULL; k++) {
                if (glyph_glob_match(install->exclude[k], orig)) {
                    excluded = true;
                    break;
                }
            }
            if (excluded) {
                continue;
            }
        }

        /* (6) strip_components, also applied to `orig`. */
        const char *stripped = orig;
        if (install != NULL && install->strip_components > 0) {
            stripped = strip_leading_components(orig,
                                                install->strip_components);
            if (stripped == NULL) {
                glyph_log_warn(
                    "skipping \"%s\": strip_components=%d consumes entire path",
                    orig, install->strip_components);
                continue;
            }
        }

        /* Build the destination path under dest_dir. */
        char *dest_path = glyph_path_join(dest_dir, stripped);
        if (dest_path == NULL) {
            goto done;
        }

        /* Ensure parent directories exist (mode 0755). */
        char *parent = derive_dirname(dest_path);
        if (parent != NULL) {
            if (glyph_mkdir_p(parent, 0755) == -1) {
                int saved_errno = errno;
                free(parent);
                free(dest_path);
                errno = saved_errno;
                goto done;
            }
            free(parent);
        }

        /* (7) Extract the file. */
        if (!mz_zip_reader_extract_to_file(&arch, i, dest_path, 0)) {
            free(dest_path);
            goto done;
        }

        /* (8) Append the malloc'd dest path to the result array. */
        if (count == cap) {
            size_t ncap = (cap == 0) ? 16 : cap * 2;
            char **narr = realloc(files, ncap * sizeof(*narr));
            if (narr == NULL) {
                free(dest_path);
                goto done;
            }
            files = narr;
            cap = ncap;
        }
        files[count++] = dest_path; /* ownership transferred */
    }

    /* (9) NULL-terminate the array (one extra slot). */
    if (count == cap) {
        size_t ncap = (cap == 0) ? 16 : cap * 2;
        char **narr = realloc(files, ncap * sizeof(*narr));
        if (narr == NULL) {
            goto done;
        }
        files = narr;
        cap = ncap;
    }
    files[count] = NULL;

    *out_files = files;
    *out_count = count;
    ok = true;

done:
    if (!ok) {
        /* (10) Free everything already collected. */
        for (size_t k = 0; k < count; k++) {
            free(files[k]);
        }
        free(files);
    }
    mz_zip_reader_end(&arch);
    return ok ? 0 : -1;
}
