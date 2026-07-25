#include "complete.h"
#include "db.h"
#include "manifest.h"
#include "util.h"
#include "test_common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char state_dir[64];   /* XDG_CACHE_HOME + XDG_DATA_HOME */
static char config_dir[64];  /* XDG_CONFIG_HOME */

static const char *FIXTURE_CATALOG =
    "{"
    "  \"fonts\": ["
    "    {\"id\": \"fira-code\", \"name\": \"Fira Code\", \"version\": \"6.2\"},"
    "    {\"id\": \"hack\", \"name\": \"Hack\", \"version\": \"3.3\"},"
    "    {\"id\": \"ibm-plex-mono\", \"name\": \"IBM Plex Mono\", \"version\": \"6.4\"}"
    "  ]"
    "}";

/* Run glyph_complete_emit with stdout captured; returns malloc'd output
 * (NUL-terminated) and sets *rc to the return value. */
static char *run_emit(const char *cmd, const char *prefix, int *rc)
{
    fflush(stdout);
    int saved = dup(STDOUT_FILENO);
    char tmpl[] = "/tmp/glyph-cap-XXXXXX";
    int fd = mkstemp(tmpl);
    ASSERT_TRUE(fd >= 0);
    dup2(fd, STDOUT_FILENO);
    close(fd);

    *rc = glyph_complete_emit(cmd, prefix);

    fflush(stdout);
    dup2(saved, STDOUT_FILENO);
    close(saved);

    char *buf = NULL;
    size_t len = 0;
    if (glyph_read_file(tmpl, &buf, &len) != 0) {
        buf = glyph_strdup("");
    }
    unlink(tmpl);
    return buf;
}

/* Line-presence check: is `line` a full line of `out`? */
static int has_line(const char *out, const char *line)
{
    size_t ll = strlen(line);
    const char *p = out;
    while ((p = strstr(p, line)) != NULL) {
        int at_start = (p == out) || (p[-1] == '\n');
        int at_end = (p[ll] == '\0') || (p[ll] == '\n');
        if (at_start && at_end) {
            return 1;
        }
        p += ll;
    }
    return 0;
}

/* 4.2: missing cache/DB -> empty output, return 0. */
static void test_missing_state(void)
{
    int rc = -1;
    char *out = run_emit("install", "", &rc);
    ASSERT_EQ(rc, 0);
    ASSERT_STR_EQ(out, "");
    free(out);

    out = run_emit("remove", "", &rc);
    ASSERT_EQ(rc, 0);
    ASSERT_STR_EQ(out, "");
    free(out);

    out = run_emit("upgrade", "x", &rc);
    ASSERT_EQ(rc, 0);
    ASSERT_STR_EQ(out, "");
    free(out);
}

/* 4.1: catalog-backed emission, prefix filtering, unknown cmd -> empty. */
static void test_catalog_emit(void)
{
    /* Seed <cache>/glyph/catalog.json with the fixture. */
    char *cdir = glyph_path_join(state_dir, "glyph");
    ASSERT_NOT_NULL(cdir);
    ASSERT_EQ(glyph_mkdir_p(cdir, 0755), 0);
    free(cdir);
    char *cpath = glyph_path_catalog_cache();
    ASSERT_NOT_NULL(cpath);
    ASSERT_EQ(glyph_write_file(cpath, FIXTURE_CATALOG,
                               strlen(FIXTURE_CATALOG), 0644), 0);
    free(cpath);

    int rc = -1;
    char *out = run_emit("install", "", &rc);
    ASSERT_EQ(rc, 0);
    ASSERT_TRUE(has_line(out, "fira-code"));
    ASSERT_TRUE(has_line(out, "hack"));
    ASSERT_TRUE(has_line(out, "ibm-plex-mono"));
    free(out);

    /* info uses the same (catalog) source. */
    out = run_emit("info", "fira", &rc);
    ASSERT_EQ(rc, 0);
    ASSERT_TRUE(has_line(out, "fira-code"));
    ASSERT_FALSE(has_line(out, "hack"));
    ASSERT_FALSE(has_line(out, "ibm-plex-mono"));
    free(out);

    /* Prefix filter excludes non-matching ids. */
    out = run_emit("install", "h", &rc);
    ASSERT_EQ(rc, 0);
    ASSERT_TRUE(has_line(out, "hack"));
    ASSERT_FALSE(has_line(out, "fira-code"));
    free(out);

    /* No match -> empty. */
    out = run_emit("install", "zzz", &rc);
    ASSERT_EQ(rc, 0);
    ASSERT_STR_EQ(out, "");
    free(out);

    /* Unknown cmd -> empty, still 0. */
    out = run_emit("bogus", "", &rc);
    ASSERT_EQ(rc, 0);
    ASSERT_STR_EQ(out, "");
    free(out);

    /* NULL cmd/prefix are tolerated. */
    out = run_emit(NULL, NULL, &rc);
    ASSERT_EQ(rc, 0);
    ASSERT_STR_EQ(out, "");
    free(out);
}

/* 4.1 (cont.): DB-backed emission for remove/upgrade. */
static void test_db_emit(void)
{
    glyph_db_t db;
    ASSERT_EQ(glyph_db_load(&db), 0);

    glyph_font_t f1, f2;
    memset(&f1, 0, sizeof(f1));
    memset(&f2, 0, sizeof(f2));
    f1.id = (char *)"fira-code";
    f1.name = (char *)"Fira Code";
    f1.version = (char *)"6.2";
    f1.revision = 1;
    f2.id = (char *)"hack";
    f2.name = (char *)"Hack";
    f2.version = (char *)"3.3";
    f2.revision = 1;

    char **files1 = malloc(2 * sizeof(char *));
    ASSERT_NOT_NULL(files1);
    files1[0] = glyph_strdup("/fonts/fira/FiraCode.otf");
    files1[1] = NULL;
    ASSERT_EQ(glyph_db_upsert(&db, &f1, files1, 1), 0);

    char **files2 = malloc(2 * sizeof(char *));
    ASSERT_NOT_NULL(files2);
    files2[0] = glyph_strdup("/fonts/hack/Hack.otf");
    files2[1] = NULL;
    ASSERT_EQ(glyph_db_upsert(&db, &f2, files2, 1), 0);

    ASSERT_EQ(glyph_db_save(&db), 0);
    glyph_db_free(&db);

    int rc = -1;
    char *out = run_emit("remove", "", &rc);
    ASSERT_EQ(rc, 0);
    ASSERT_TRUE(has_line(out, "fira-code"));
    ASSERT_TRUE(has_line(out, "hack"));
    free(out);

    out = run_emit("upgrade", "fir", &rc);
    ASSERT_EQ(rc, 0);
    ASSERT_TRUE(has_line(out, "fira-code"));
    ASSERT_FALSE(has_line(out, "hack"));
    free(out);

    out = run_emit("remove", "zzz", &rc);
    ASSERT_EQ(rc, 0);
    ASSERT_STR_EQ(out, "");
    free(out);

    /* install/info still come from the catalog, not the DB. */
    out = run_emit("install", "hack", &rc);
    ASSERT_EQ(rc, 0);
    ASSERT_TRUE(has_line(out, "hack"));
    free(out);
}

/* 4.3: embedded fish script registers completions for glyph. */
static void test_fish_script(void)
{
    const char *s = glyph_complete_fish_script();
    ASSERT_NOT_NULL(s);
    ASSERT_NOT_NULL(strstr(s, "complete -c glyph"));
    /* Layer 1 subcommands, layer 2 actions/flags, layer 3 backend. */
    ASSERT_NOT_NULL(strstr(s, "-a install"));
    ASSERT_NOT_NULL(strstr(s, "-a remove"));
    ASSERT_NOT_NULL(strstr(s, "-a completions"));
    ASSERT_NOT_NULL(strstr(s, "-a update"));
    ASSERT_NOT_NULL(strstr(s, "-a status"));
    ASSERT_NOT_NULL(strstr(s, "-l no-cache"));
    ASSERT_NOT_NULL(strstr(s, "-l all"));
    ASSERT_NOT_NULL(strstr(s, "-l catalog"));
    ASSERT_NOT_NULL(strstr(s, "glyph __complete"));
}

/* 4.4: self-install into XDG_CONFIG_HOME, with atomic overwrite. */
static void test_install(void)
{
    setenv("XDG_CONFIG_HOME", config_dir, 1);

    char *path = NULL;
    ASSERT_EQ(glyph_complete_install_fish(&path), 0);
    ASSERT_NOT_NULL(path);

    char *want_dir = glyph_path_join(config_dir, "fish/completions");
    char *want = glyph_path_join(want_dir, "glyph.fish");
    ASSERT_STR_EQ(path, want);

    /* Written content matches the embedded script byte-for-byte. */
    const char *script = glyph_complete_fish_script();
    char *got = NULL;
    size_t got_len = 0;
    ASSERT_EQ(glyph_read_file(path, &got, &got_len), 0);
    ASSERT_EQ((long)got_len, (long)strlen(script));
    ASSERT_STR_EQ(got, script);
    free(got);

    /* Corrupt, then re-install: overwrite must restore the script. */
    ASSERT_EQ(glyph_write_file(path, "garbage", 7, 0644), 0);
    free(path);
    path = NULL;
    ASSERT_EQ(glyph_complete_install_fish(&path), 0);
    ASSERT_EQ(glyph_read_file(path, &got, &got_len), 0);
    ASSERT_STR_EQ(got, script);
    free(got);

    free(path);
    free(want);
    free(want_dir);
}

int main(void)
{
    snprintf(state_dir, sizeof(state_dir), "/tmp/glyph-cpl-XXXXXX");
    ASSERT_NOT_NULL(mkdtemp(state_dir));
    snprintf(config_dir, sizeof(config_dir), "/tmp/glyph-cfg-XXXXXX");
    ASSERT_NOT_NULL(mkdtemp(config_dir));
    setenv("XDG_CACHE_HOME", state_dir, 1);
    setenv("XDG_DATA_HOME", state_dir, 1);

    test_missing_state();
    test_catalog_emit();
    test_db_emit();
    test_fish_script();
    test_install();

    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s' '%s'", state_dir, config_dir);
    int rc = system(cmd);
    (void)rc;

    TEST_SUMMARY();
    return test_failures > 0 ? 1 : 0;
}
