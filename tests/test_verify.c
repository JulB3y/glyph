#include "verify.h"
#include "util.h"
#include "test_common.h"

#include <stdint.h>
#include <unistd.h>

/* SHA-256 of the embedded DER pubkey (res/glyph.pem), as declared in the
 * release v0.1.0 fixture catalog's meta.signature_fingerprint. */
#define FIXTURE_FP_HEX \
    "fa847fb00d32331edf837100780d836c02b6b4c76232e12f969edc7f222bdf26"
#define FIXTURE_FP_COLON \
    "fa:84:7f:b0:0d:32:33:1e:df:83:71:00:78:0d:83:6c:02:b6:b4:c7:62:" \
    "32:e1:2f:96:9e:dc:7f:22:2b:df:26"

/* Resolve tests/fixtures/<name> relative to the meson source root (tests run
 * with MESON_SOURCE_ROOT set; fall back to ".." when run from build/). */
static char *fixture_path(const char *name)
{
    const char *root = getenv("MESON_SOURCE_ROOT");
    if (root == NULL || root[0] == '\0') {
        root = "..";
    }
    size_t n = strlen(root) + strlen(name) + 32;
    char *p = malloc(n);
    if (p != NULL) {
        snprintf(p, n, "%s/tests/fixtures/%s", root, name);
    }
    return p;
}

/* Load both release v0.1.0 fixtures; caller frees all four. */
static void load_fixtures(char **cat, size_t *cat_len,
                          char **sig, size_t *sig_len)
{
    char *cp = fixture_path("catalog.json");
    char *sp = fixture_path("catalog.json.sig2");
    ASSERT_NOT_NULL(cp);
    ASSERT_NOT_NULL(sp);
    ASSERT_EQ(glyph_read_file(cp, cat, cat_len), 0);
    ASSERT_EQ(glyph_read_file(sp, sig, sig_len), 0);
    free(cp);
    free(sp);
}

static void test_sha256_empty(void)
{
    char hex[65];
    int rc = glyph_sha256_buf("", 0, hex);
    ASSERT_EQ(rc, 0);
    ASSERT_STR_EQ(hex,
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
}

static void test_sha256_abc(void)
{
    char hex[65];
    int rc = glyph_sha256_buf("abc", 3, hex);
    ASSERT_EQ(rc, 0);
    ASSERT_STR_EQ(hex,
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

static void test_sha256_multiblock(void)
{
    const char *msg = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    char hex[65];
    int rc = glyph_sha256_buf(msg, strlen(msg), hex);
    ASSERT_EQ(rc, 0);
    ASSERT_STR_EQ(hex,
        "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
}

static void test_sha256_file(void)
{
    char tmpl[] = "/tmp/glyph-test-sha-XXXXXX";
    int fd = mkstemp(tmpl);
    ASSERT_TRUE(fd >= 0);
    if (fd < 0) return;
    ssize_t w = write(fd, "abc", 3);
    ASSERT_EQ(w, 3);
    close(fd);

    char hex[65];
    int rc = glyph_sha256_file(tmpl, hex);
    ASSERT_EQ(rc, 0);
    ASSERT_STR_EQ(hex,
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");

    unlink(tmpl);
}

static void test_sha256_verify_file(void)
{
    char tmpl[] = "/tmp/glyph-test-vfy-XXXXXX";
    int fd = mkstemp(tmpl);
    ASSERT_TRUE(fd >= 0);
    if (fd < 0) return;
    ssize_t w = write(fd, "abc", 3);
    ASSERT_EQ(w, 3);
    close(fd);

    /* exact match */
    ASSERT_TRUE(glyph_sha256_verify_file(tmpl,
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    /* uppercase should match (case-insensitive) */
    ASSERT_TRUE(glyph_sha256_verify_file(tmpl,
        "BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD"));
    /* wrong hash */
    ASSERT_FALSE(glyph_sha256_verify_file(tmpl,
        "0000000000000000000000000000000000000000000000000000000000000000"));

    unlink(tmpl);
}

static void test_embedded_key(void)
{
    size_t len = 0;
    const uint8_t *der = glyph_embedded_pubkey_der(&len);
    ASSERT_NOT_NULL(der);
    ASSERT_EQ((long)len, 294); /* RSA-2048 SubjectPublicKeyInfo */

    char fp[65];
    ASSERT_EQ(glyph_embedded_pubkey_fingerprint(fp), 0);
    ASSERT_STR_EQ(fp, FIXTURE_FP_HEX);
}

static void test_catalog_signature_valid(void)
{
    char *cat = NULL, *sig = NULL;
    size_t cat_len = 0, sig_len = 0;
    load_fixtures(&cat, &cat_len, &sig, &sig_len);
    ASSERT_EQ((long)sig_len, 256); /* exactly RSA-2048 */

    /* Declared fingerprint as it appears in the catalog (colon-separated). */
    ASSERT_EQ(glyph_verify_catalog_signature(cat, cat_len,
                                             (const uint8_t *)sig, sig_len,
                                             FIXTURE_FP_COLON), 0);
    /* Normalization: bare lowercase hex and bare uppercase hex both match. */
    ASSERT_EQ(glyph_verify_catalog_signature(cat, cat_len,
                                             (const uint8_t *)sig, sig_len,
                                             FIXTURE_FP_HEX), 0);
    ASSERT_EQ(glyph_verify_catalog_signature(
                  cat, cat_len, (const uint8_t *)sig, sig_len,
                  "FA847FB00D32331EDF837100780D836C02B6B4C76232E12F"
                  "969EDC7F222BDF26"), 0);

    free(cat);
    free(sig);
}

static void test_catalog_signature_tampered(void)
{
    char *cat = NULL, *sig = NULL;
    size_t cat_len = 0, sig_len = 0;
    load_fixtures(&cat, &cat_len, &sig, &sig_len);

    /* Flip one byte of the catalog: fingerprint still matches, but the RSA
     * verification over the exact bytes must fail -> -1 (exit 4 in main). */
    cat[cat_len / 2] ^= 0x01;
    ASSERT_EQ(glyph_verify_catalog_signature(cat, cat_len,
                                             (const uint8_t *)sig, sig_len,
                                             FIXTURE_FP_COLON), -1);

    free(cat);
    free(sig);
}

static void test_catalog_signature_fp_mismatch(void)
{
    char *cat = NULL, *sig = NULL;
    size_t cat_len = 0, sig_len = 0;
    load_fixtures(&cat, &cat_len, &sig, &sig_len);

    /* Declared fingerprint differs from the embedded key -> -2 with a
     * key-rotation diagnostic, before RSA is even attempted. */
    ASSERT_EQ(glyph_verify_catalog_signature(
                  cat, cat_len, (const uint8_t *)sig, sig_len,
                  "00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:"
                  "00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00"), -2);
    /* Missing declaration is a mismatch too (fail closed). */
    ASSERT_EQ(glyph_verify_catalog_signature(cat, cat_len,
                                             (const uint8_t *)sig, sig_len,
                                             NULL), -2);

    free(cat);
    free(sig);
}

int main(void)
{
    test_sha256_empty();
    test_sha256_abc();
    test_sha256_multiblock();
    test_sha256_file();
    test_sha256_verify_file();
    test_embedded_key();
    test_catalog_signature_valid();
    test_catalog_signature_tampered();
    test_catalog_signature_fp_mismatch();
    TEST_SUMMARY();
    return test_failures > 0 ? 1 : 0;
}
