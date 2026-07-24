#include "db.h"
#include "manifest.h"
#include "util.h"
#include "test_common.h"

#include <unistd.h>
#include <sys/stat.h>

static char data_dir[256];

static void setup_env(void)
{
    snprintf(data_dir, sizeof(data_dir), "/tmp/glyph-db-XXXXXX");
    ASSERT_NOT_NULL(mkdtemp(data_dir));
    setenv("XDG_DATA_HOME", data_dir, 1);
}

static void test_load_empty(void)
{
    glyph_db_t db;
    int rc = glyph_db_load(&db);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ((long)db.n_fonts, 0);
    ASSERT_NULL(db.fonts);
    glyph_db_free(&db);
}

static glyph_font_t make_font(const char *id, const char *name,
                               const char *version)
{
    glyph_font_t f;
    memset(&f, 0, sizeof(f));
    f.id = (char *)id;
    f.name = (char *)name;
    f.version = (char *)version;
    f.revision = 1;
    return f;
}

static void test_upsert_find(void)
{
    glyph_db_t db;
    ASSERT_EQ(glyph_db_load(&db), 0);

    glyph_font_t font = make_font("inter", "Inter", "4.0");
    char **files = malloc(2 * sizeof(char *));
    ASSERT_NOT_NULL(files);
    files[0] = glyph_strdup("/fonts/inter/Inter.otf");
    files[1] = glyph_strdup("/fonts/inter/Inter-Bold.otf");

    int rc = glyph_db_upsert(&db, &font, files, 2);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ((long)db.n_fonts, 1);

    const glyph_installed_font_t *found = glyph_db_find(&db, "inter");
    ASSERT_NOT_NULL(found);
    ASSERT_STR_EQ(found->id, "inter");
    ASSERT_STR_EQ(found->name, "Inter");
    ASSERT_STR_EQ(found->version, "4.0");
    ASSERT_EQ(found->revision, 1);
    ASSERT_NOT_NULL(found->files);
    ASSERT_STR_EQ(found->files[0], "/fonts/inter/Inter.otf");
    ASSERT_STR_EQ(found->files[1], "/fonts/inter/Inter-Bold.otf");
    ASSERT_NULL(found->files[2]);

    ASSERT_NULL(glyph_db_find(&db, "nonexistent"));

    glyph_db_free(&db);
}

static void test_remove(void)
{
    glyph_db_t db;
    ASSERT_EQ(glyph_db_load(&db), 0);

    glyph_font_t font = make_font("fira", "Fira Code", "6.0");
    char **files = malloc(1 * sizeof(char *));
    files[0] = glyph_strdup("/fonts/fira/FiraCode.otf");
    ASSERT_EQ(glyph_db_upsert(&db, &font, files, 1), 0);
    ASSERT_EQ((long)db.n_fonts, 1);

    ASSERT_EQ(glyph_db_remove(&db, "fira"), 0);
    ASSERT_EQ((long)db.n_fonts, 0);
    ASSERT_NULL(glyph_db_find(&db, "fira"));

    /* removing nonexistent returns -1 */
    ASSERT_EQ(glyph_db_remove(&db, "ghost"), -1);

    glyph_db_free(&db);
}

static void test_save_reload(void)
{
    glyph_db_t db;
    ASSERT_EQ(glyph_db_load(&db), 0);

    glyph_font_t font = make_font("jetbrains", "JetBrains Mono", "2.3");
    char **files = malloc(1 * sizeof(char *));
    files[0] = glyph_strdup("/fonts/jb/JBMono.ttf");
    ASSERT_EQ(glyph_db_upsert(&db, &font, files, 1), 0);

    ASSERT_EQ(glyph_db_save(&db), 0);
    glyph_db_free(&db);

    /* reload from disk */
    glyph_db_t db2;
    ASSERT_EQ(glyph_db_load(&db2), 0);
    ASSERT_EQ((long)db2.n_fonts, 1);

    const glyph_installed_font_t *f = glyph_db_find(&db2, "jetbrains");
    ASSERT_NOT_NULL(f);
    ASSERT_STR_EQ(f->name, "JetBrains Mono");
    ASSERT_STR_EQ(f->version, "2.3");
    ASSERT_EQ(f->revision, 1);
    ASSERT_NOT_NULL(f->files);
    ASSERT_STR_EQ(f->files[0], "/fonts/jb/JBMono.ttf");

    glyph_db_free(&db2);
}

int main(void)
{
    setup_env();

    test_load_empty();
    test_upsert_find();
    test_remove();
    test_save_reload();

    /* cleanup */
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", data_dir);
    int rc = system(cmd);
    (void)rc;

    TEST_SUMMARY();
    return test_failures > 0 ? 1 : 0;
}
