/*
 * tests/test_util.c — release-tag sidecar + catalog cache freshness.
 *
 * Covers the unit-testable pieces behind `index update`'s skip path and
 * `index status` tag reporting:
 *   - glyph_path_release_tag() composition
 *   - glyph_write_release_tag() / glyph_read_release_tag() round-trip
 *   - missing sidecar tolerated (read -> NULL, never an error)
 *   - glyph_catalog_cache_is_current(): unchanged tag + complete cache ->
 *     true (update skips the asset downloads); new tag, missing cache file,
 *     or missing sidecar -> false (update re-downloads / status shows
 *     "(unknown)").
 */

#include "util.h"
#include "test_common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char *g_tmp; /* per-test XDG_CACHE_HOME, rm -rf'd by the shell */

static void fresh_cache_home(void)
{
    char tmpl[] = "/tmp/glyph-test-util-XXXXXX";
    ASSERT_NOT_NULL(mkdtemp(tmpl));
    g_tmp = glyph_strdup(tmpl);
    ASSERT_NOT_NULL(g_tmp);
    ASSERT_EQ(setenv("XDG_CACHE_HOME", g_tmp, 1), 0);
}

static void test_release_tag_path(void)
{
    fresh_cache_home();
    char *p = glyph_path_release_tag();
    ASSERT_NOT_NULL(p);
    ASSERT_TRUE(glyph_str_ends_with(p, "/glyph/release-tag"));
    ASSERT_TRUE(glyph_str_starts_with(p, g_tmp));
    free(p);
}

static void test_release_tag_roundtrip(void)
{
    fresh_cache_home();

    /* Missing sidecar is tolerated: read yields NULL, not an error. This is
     * what makes `index status` print "(unknown)" on a cache without a tag. */
    ASSERT_NULL(glyph_read_release_tag());

    ASSERT_EQ(glyph_write_release_tag("v0.1.0"), 0);
    char *t = glyph_read_release_tag();
    ASSERT_NOT_NULL(t);
    ASSERT_STR_EQ(t, "v0.1.0"); /* trailing newline trimmed */
    free(t);

    /* Overwrite atomically. */
    ASSERT_EQ(glyph_write_release_tag("v0.2.0"), 0);
    t = glyph_read_release_tag();
    ASSERT_NOT_NULL(t);
    ASSERT_STR_EQ(t, "v0.2.0");
    free(t);

    /* Empty/NULL tags are rejected. */
    ASSERT_EQ(glyph_write_release_tag(""), -1);
    ASSERT_EQ(glyph_write_release_tag(NULL), -1);
}

/* Create the two cached catalog files (non-empty) under the temp cache. */
static void create_cache_files(void)
{
    char *cp = glyph_path_catalog_cache();
    char *sp = glyph_path_catalog_sig_cache();
    ASSERT_NOT_NULL(cp);
    ASSERT_NOT_NULL(sp);
    /* glyph_write_release_tag() already mkdir -p'd <cache>/glyph. */
    ASSERT_EQ(glyph_write_file(cp, "{\"meta\":{}}", 11, 0644), 0);
    ASSERT_EQ(glyph_write_file(sp, "sig", 3, 0644), 0);
    free(cp);
    free(sp);
}

static void test_cache_is_current(void)
{
    fresh_cache_home();

    /* Nothing cached at all -> not current (status: "no catalog cached"). */
    ASSERT_FALSE(glyph_catalog_cache_is_current("v0.1.0"));

    /* Tag written but catalog files missing -> not current. */
    ASSERT_EQ(glyph_write_release_tag("v0.1.0"), 0);
    ASSERT_FALSE(glyph_catalog_cache_is_current("v0.1.0"));

    create_cache_files();

    /* Unchanged tag + both files present -> current: `index update` skips
     * the two asset downloads and exits 0. */
    ASSERT_TRUE(glyph_catalog_cache_is_current("v0.1.0"));

    /* New tag -> not current: `index update` re-downloads. */
    ASSERT_FALSE(glyph_catalog_cache_is_current("v0.2.0"));

    /* One cache file missing -> not current even with a matching tag. */
    char *sp = glyph_path_catalog_sig_cache();
    ASSERT_NOT_NULL(sp);
    ASSERT_EQ(unlink(sp), 0);
    ASSERT_FALSE(glyph_catalog_cache_is_current("v0.1.0"));
    ASSERT_EQ(glyph_write_file(sp, "sig", 3, 0644), 0);
    free(sp);
    ASSERT_TRUE(glyph_catalog_cache_is_current("v0.1.0"));

    /* Missing sidecar tolerated -> not current (no crash, no error). */
    char *tp = glyph_path_release_tag();
    ASSERT_NOT_NULL(tp);
    ASSERT_EQ(unlink(tp), 0);
    ASSERT_FALSE(glyph_catalog_cache_is_current("v0.1.0"));
    ASSERT_NULL(glyph_read_release_tag());
    free(tp);

    /* NULL/empty tag -> never current. */
    ASSERT_FALSE(glyph_catalog_cache_is_current(NULL));
    ASSERT_FALSE(glyph_catalog_cache_is_current(""));
}

int main(void)
{
    test_release_tag_path();
    test_release_tag_roundtrip();
    test_cache_is_current();
    TEST_SUMMARY();
    return test_failures > 0 ? 1 : 0;
}
