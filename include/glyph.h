#ifndef GLYPH_H
#define GLYPH_H

#include <stddef.h>
#include <stdbool.h>

#define GLYPH_VERSION "0.1.0"
#define GLYPH_USER_AGENT "glyph/0.1"
#define GLYPH_DB_FORMAT 1

/* Catalog discovery endpoint: the latest release of the glyph-catalog GitHub
 * repo. The signed catalog exists ONLY as a release asset, never in branches;
 * `index update` resolves the `catalog.json` / `catalog.json.sig2` assets by
 * exact name from this release object. There is no URL override in v1. */
#define GLYPH_CATALOG_RELEASES_API \
    "https://api.github.com/repos/JulB3y/glyph-catalog/releases/latest"

/* Exit codes (see pln-c-repo.md section 8). */
enum {
    GLYPH_EXIT_OK                = 0,
    GLYPH_EXIT_ERROR             = 1,
    GLYPH_EXIT_USAGE             = 2,
    GLYPH_EXIT_NETWORK           = 3,
    GLYPH_EXIT_INTEGRITY         = 4,
    GLYPH_EXIT_NOT_FOUND         = 5,
    GLYPH_EXIT_ALREADY_INSTALLED = 6,
    GLYPH_EXIT_LOCK              = 7,
    GLYPH_EXIT_MISSING_DEP       = 8,
};

/* Allowed URL schemes for downloads (HTTPS-only). NULL-terminated. */
extern const char *const GLYPH_ALLOWED_SCHEMES[];

#endif /* GLYPH_H */
