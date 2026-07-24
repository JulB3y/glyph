#include "extract.h"
#include "util.h"
#include "test_common.h"

#include <miniz.h>
#include <sys/stat.h>
#include <unistd.h>

/* ---- path safety ---- */

static void test_path_is_safe(void)
{
    ASSERT_FALSE(glyph_path_is_safe("../etc/passwd"));
    ASSERT_FALSE(glyph_path_is_safe("fonts/../../../etc/passwd"));
    ASSERT_FALSE(glyph_path_is_safe("a/../../b"));
    ASSERT_FALSE(glyph_path_is_safe(".."));
    ASSERT_FALSE(glyph_path_is_safe("/etc/passwd"));
    ASSERT_FALSE(glyph_path_is_safe("/"));
    ASSERT_FALSE(glyph_path_is_safe(""));
    ASSERT_FALSE(glyph_path_is_safe(NULL));

    ASSERT_TRUE(glyph_path_is_safe("fonts/inter/Inter-Regular.otf"));
    ASSERT_TRUE(glyph_path_is_safe("a.otf"));
    ASSERT_TRUE(glyph_path_is_safe("./a/b.ttf"));
    ASSERT_TRUE(glyph_path_is_safe("a/b/c"));
}

/* ---- glob ---- */

static void test_glob(void)
{
    ASSERT_TRUE(glyph_glob_match("*.otf", "Inter-Regular.otf"));
    ASSERT_TRUE(glyph_glob_match("*.otf", ".otf"));
    ASSERT_FALSE(glyph_glob_match("*.otf", "Inter.ttf"));
    ASSERT_TRUE(glyph_glob_match("font?.ttf", "fonts.ttf"));
    ASSERT_FALSE(glyph_glob_match("font?.ttf", "font.ttf"));
    ASSERT_FALSE(glyph_glob_match("font?.ttf", "fontss.ttf"));
    ASSERT_TRUE(glyph_glob_match("*", "anything"));
    ASSERT_TRUE(glyph_glob_match("*", ""));
    ASSERT_TRUE(glyph_glob_match("a*c", "abc"));
    ASSERT_TRUE(glyph_glob_match("a*c", "ac"));
    ASSERT_FALSE(glyph_glob_match("a*c", "abd"));
    ASSERT_FALSE(glyph_glob_match(NULL, "x"));
    ASSERT_FALSE(glyph_glob_match("x", NULL));
}

/* ---- helpers ---- */

static char test_dir[256];

static void make_test_dir(void)
{
    snprintf(test_dir, sizeof(test_dir), "/tmp/glyph-ext-XXXXXX");
    ASSERT_NOT_NULL(mkdtemp(test_dir));
}

static void rm_rf_cb(const char *path)
{
    /* best-effort recursive remove via system() — test-only */
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", path);
    int rc = system(cmd);
    (void)rc;
}

/* Build a ZIP at `zip_path` containing the given entries. */
static int make_zip(const char *zip_path, const char **names,
                    const char **contents, int n)
{
    mz_zip_archive zip;
    mz_zip_zero_struct(&zip);
    if (!mz_zip_writer_init_file(&zip, zip_path, 0))
        return -1;
    for (int i = 0; i < n; i++) {
        if (!mz_zip_writer_add_mem(&zip, names[i], contents[i],
                                   strlen(contents[i]),
                                   MZ_DEFAULT_COMPRESSION)) {
            mz_zip_writer_end(&zip);
            return -1;
        }
    }
    mz_zip_writer_finalize_archive(&zip);
    mz_zip_writer_end(&zip);
    return 0;
}

/* ---- extraction tests ---- */

static void test_extract_all(void)
{
    char zip_path[300], dest[300];
    snprintf(zip_path, sizeof(zip_path), "%s/all.zip", test_dir);
    snprintf(dest, sizeof(dest), "%s/out_all", test_dir);

    const char *names[] = {"a.otf", "b.ttf"};
    const char *data[]  = {"font-a", "font-b"};
    ASSERT_EQ(make_zip(zip_path, names, data, 2), 0);

    char **files = NULL;
    size_t count = 0;
    int rc = glyph_extract_zip(zip_path, dest, NULL, &files, &count);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ((long)count, 2);
    ASSERT_NOT_NULL(files);

    /* verify files exist on disk */
    for (size_t i = 0; i < count; i++) {
        struct stat st;
        ASSERT_EQ(stat(files[i], &st), 0);
        free(files[i]);
    }
    free(files);
}

static void test_extract_strip(void)
{
    char zip_path[300], dest[300];
    snprintf(zip_path, sizeof(zip_path), "%s/strip.zip", test_dir);
    snprintf(dest, sizeof(dest), "%s/out_strip", test_dir);

    const char *names[] = {"pkg/fonts/a.otf"};
    const char *data[]  = {"stripped"};
    ASSERT_EQ(make_zip(zip_path, names, data, 1), 0);

    glyph_install_t inst;
    memset(&inst, 0, sizeof(inst));
    inst.strip_components = 2;

    char **files = NULL;
    size_t count = 0;
    int rc = glyph_extract_zip(zip_path, dest, &inst, &files, &count);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ((long)count, 1);

    /* file should be at dest/a.otf */
    char expected[400];
    snprintf(expected, sizeof(expected), "%s/a.otf", dest);
    ASSERT_STR_EQ(files[0], expected);
    struct stat st;
    ASSERT_EQ(stat(files[0], &st), 0);

    free(files[0]);
    free(files);
}

static void test_extract_include_filter(void)
{
    char zip_path[300], dest[300];
    snprintf(zip_path, sizeof(zip_path), "%s/filter.zip", test_dir);
    snprintf(dest, sizeof(dest), "%s/out_filter", test_dir);

    const char *names[] = {"a.otf", "b.txt"};
    const char *data[]  = {"font", "text"};
    ASSERT_EQ(make_zip(zip_path, names, data, 1 + 1), 0);

    char *inc[] = {"*.otf", NULL};
    glyph_install_t inst;
    memset(&inst, 0, sizeof(inst));
    inst.include = inc;

    char **files = NULL;
    size_t count = 0;
    int rc = glyph_extract_zip(zip_path, dest, &inst, &files, &count);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ((long)count, 1);

    /* only a.otf extracted */
    ASSERT_TRUE(glyph_str_ends_with(files[0], "a.otf"));

    free(files[0]);
    free(files);
}

static void test_extract_traversal(void)
{
    char zip_path[300], dest[300];
    snprintf(zip_path, sizeof(zip_path), "%s/evil.zip", test_dir);
    snprintf(dest, sizeof(dest), "%s/out_evil", test_dir);

    const char *names[] = {"../../evil.ttf"};
    const char *data[]  = {"pwned"};
    ASSERT_EQ(make_zip(zip_path, names, data, 1), 0);

    char **files = NULL;
    size_t count = 0;
    int rc = glyph_extract_zip(zip_path, dest, NULL, &files, &count);
    /* unsafe entries are skipped; extraction succeeds with 0 files */
    ASSERT_EQ(rc, 0);
    ASSERT_EQ((long)count, 0);

    /* evil file must NOT exist outside dest */
    char evil[400];
    snprintf(evil, sizeof(evil), "%s/../../evil.ttf", dest);
    struct stat st;
    ASSERT_TRUE(stat(evil, &st) != 0);

    free(files);
}

int main(void)
{
    make_test_dir();

    test_path_is_safe();
    test_glob();
    test_extract_all();
    test_extract_strip();
    test_extract_include_filter();
    test_extract_traversal();

    rm_rf_cb(test_dir);
    TEST_SUMMARY();
    return test_failures > 0 ? 1 : 0;
}
