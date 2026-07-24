#include "manifest.h"
#include "test_common.h"

static const char *VALID_CATALOG =
    "{"
    "  \"meta\": {"
    "    \"version\": \"1.0\","
    "    \"last_updated\": \"2025-01-01T00:00:00Z\","
    "    \"count\": 1,"
    "    \"signature_fingerprint\": \"ABCD1234\""
    "  },"
    "  \"fonts\": ["
    "    {"
    "      \"id\": \"inter\","
    "      \"name\": \"Inter\","
    "      \"author\": \"Rasmus Andersson\","
    "      \"license\": \"OFL-1.1\","
    "      \"category\": \"sans-serif\","
    "      \"description\": \"A typeface for screens\","
    "      \"version\": \"4.0\","
    "      \"revision\": 2,"
    "      \"source\": {"
    "        \"url\": \"https://example.com/inter.zip\","
    "        \"sha256\": \"abc123\","
    "        \"format\": \"zip\""
    "      },"
    "      \"install\": {"
    "        \"strip_components\": 1,"
    "        \"include\": [\"*.otf\", \"*.ttf\"],"
    "        \"exclude\": [\"*.txt\"]"
    "      },"
    "      \"homepage\": \"https://rsms.me/inter/\","
    "      \"tags\": [\"sans\", \"variable\"]"
    "    }"
    "  ]"
    "}";

static const char *MULTI_CATALOG =
    "{"
    "  \"fonts\": ["
    "    {\"id\": \"a\", \"name\": \"A\", \"version\": \"1.0\"},"
    "    {\"id\": \"b\", \"name\": \"B\", \"version\": \"2.0\"},"
    "    {\"id\": \"c\", \"name\": \"C\", \"version\": \"3.0\"}"
    "  ]"
    "}";

static const char *MINIMAL_CATALOG =
    "{"
    "  \"fonts\": ["
    "    {\"id\": \"bare\", \"name\": \"Bare\", \"version\": \"0.1\"}"
    "  ]"
    "}";

static void test_parse_valid(void)
{
    glyph_catalog_t cat;
    int rc = glyph_catalog_parse(VALID_CATALOG, &cat);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ((long)cat.n_fonts, 1);
    ASSERT_STR_EQ(cat.version, "1.0");
    ASSERT_STR_EQ(cat.last_updated, "2025-01-01T00:00:00Z");
    ASSERT_STR_EQ(cat.signature_fingerprint, "ABCD1234");
    ASSERT_EQ(cat.count, 1);

    const glyph_font_t *f = &cat.fonts[0];
    ASSERT_STR_EQ(f->id, "inter");
    ASSERT_STR_EQ(f->name, "Inter");
    ASSERT_STR_EQ(f->author, "Rasmus Andersson");
    ASSERT_STR_EQ(f->license, "OFL-1.1");
    ASSERT_STR_EQ(f->category, "sans-serif");
    ASSERT_STR_EQ(f->description, "A typeface for screens");
    ASSERT_STR_EQ(f->version, "4.0");
    ASSERT_EQ(f->revision, 2);
    ASSERT_STR_EQ(f->source.url, "https://example.com/inter.zip");
    ASSERT_STR_EQ(f->source.sha256, "abc123");
    ASSERT_STR_EQ(f->source.format, "zip");
    ASSERT_EQ(f->install.strip_components, 1);
    ASSERT_NOT_NULL(f->install.include);
    ASSERT_STR_EQ(f->install.include[0], "*.otf");
    ASSERT_STR_EQ(f->install.include[1], "*.ttf");
    ASSERT_NULL(f->install.include[2]);
    ASSERT_NOT_NULL(f->install.exclude);
    ASSERT_STR_EQ(f->install.exclude[0], "*.txt");
    ASSERT_NULL(f->install.exclude[1]);
    ASSERT_STR_EQ(f->homepage, "https://rsms.me/inter/");
    ASSERT_NOT_NULL(f->tags);
    ASSERT_STR_EQ(f->tags[0], "sans");
    ASSERT_STR_EQ(f->tags[1], "variable");
    ASSERT_NULL(f->tags[2]);

    glyph_catalog_free(&cat);
}

static void test_parse_multi(void)
{
    glyph_catalog_t cat;
    int rc = glyph_catalog_parse(MULTI_CATALOG, &cat);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ((long)cat.n_fonts, 3);
    ASSERT_STR_EQ(cat.fonts[0].id, "a");
    ASSERT_STR_EQ(cat.fonts[1].id, "b");
    ASSERT_STR_EQ(cat.fonts[2].id, "c");
    glyph_catalog_free(&cat);
}

static void test_parse_malformed(void)
{
    glyph_catalog_t cat;
    ASSERT_TRUE(glyph_catalog_parse("{invalid", &cat) < 0);
    ASSERT_TRUE(glyph_catalog_parse("", &cat) < 0);
    ASSERT_TRUE(glyph_catalog_parse(NULL, &cat) < 0);
}

static void test_parse_missing_optional(void)
{
    glyph_catalog_t cat;
    int rc = glyph_catalog_parse(MINIMAL_CATALOG, &cat);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ((long)cat.n_fonts, 1);
    const glyph_font_t *f = &cat.fonts[0];
    ASSERT_STR_EQ(f->id, "bare");
    ASSERT_NULL(f->author);
    ASSERT_NULL(f->homepage);
    ASSERT_NULL(f->tags);
    ASSERT_NULL(f->source.url);
    ASSERT_NULL(f->install.include);
    ASSERT_NULL(f->install.exclude);
    ASSERT_EQ(f->install.strip_components, 0);
    ASSERT_EQ(f->revision, 0);
    glyph_catalog_free(&cat);
}

static void test_find(void)
{
    glyph_catalog_t cat;
    int rc = glyph_catalog_parse(MULTI_CATALOG, &cat);
    ASSERT_EQ(rc, 0);

    const glyph_font_t *f = glyph_catalog_find(&cat, "b");
    ASSERT_NOT_NULL(f);
    ASSERT_STR_EQ(f->id, "b");
    ASSERT_STR_EQ(f->name, "B");

    ASSERT_NULL(glyph_catalog_find(&cat, "z"));
    ASSERT_NULL(glyph_catalog_find(&cat, ""));
    ASSERT_NULL(glyph_catalog_find(NULL, "a"));

    glyph_catalog_free(&cat);
}

int main(void)
{
    test_parse_valid();
    test_parse_multi();
    test_parse_malformed();
    test_parse_missing_optional();
    test_find();
    TEST_SUMMARY();
    return test_failures > 0 ? 1 : 0;
}
