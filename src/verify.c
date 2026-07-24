/*
 * src/verify.c - SHA-256, RSA-2048 PKCS#1 v1.5 verify, embedded trust anchor.
 *
 * Compiles under:
 *   gcc -std=c99 -Wall -Wextra -Werror -Wstrict-prototypes -Wmissing-prototypes
 *
 * Only libc; no external crypto. Feature-test macros come first so every
 * system header sees them.
 *
 * The verifying key is the DER SubjectPublicKeyInfo generated at build time
 * from res/glyph.pem (glyph_pubkey_der.h). No key material is ever read from
 * disk or fetched from the network.
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

#include "verify.h"
#include "glyph.h"
#include "util.h"

#include "glyph_pubkey_der.h" /* generated at build time from res/glyph.pem */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <ctype.h>

/* ---- SHA-256 (FIPS 180-4) ---- */

static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
    0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

static uint32_t rotr(uint32_t x, int n)
{
    return (x >> n) | (x << (32 - n));
}

void glyph_sha256_init(glyph_sha256_ctx *c)
{
    if (c == NULL) {
        return;
    }
    c->state[0] = 0x6a09e667;
    c->state[1] = 0xbb67ae85;
    c->state[2] = 0x3c6ef372;
    c->state[3] = 0xa54ff53a;
    c->state[4] = 0x510e527f;
    c->state[5] = 0x9b05688c;
    c->state[6] = 0x1f83d9ab;
    c->state[7] = 0x5be0cd19;
    c->bitlen = 0;
    c->datalen = 0;
}

static void sha256_transform(glyph_sha256_ctx *c, const uint8_t data[64])
{
    /* = {0}: every entry is written before use, but the zero-init silences
     * static-analyzer false positives on the split init loops below. */
    uint32_t W[64] = {0};
    int i;

    /* Big-endian load of the first 16 words. */
    for (i = 0; i < 16; i++) {
        W[i] = ((uint32_t)data[i * 4] << 24)
             | ((uint32_t)data[i * 4 + 1] << 16)
             | ((uint32_t)data[i * 4 + 2] << 8)
             | ((uint32_t)data[i * 4 + 3]);
    }
    /* Message schedule extension. */
    for (i = 16; i < 64; i++) {
        uint32_t s0 = rotr(W[i - 15], 7) ^ rotr(W[i - 15], 18) ^ (W[i - 15] >> 3);
        uint32_t s1 = rotr(W[i - 2], 17) ^ rotr(W[i - 2], 19) ^ (W[i - 2] >> 10);
        W[i] = W[i - 16] + s0 + W[i - 7] + s1;
    }

    uint32_t a = c->state[0], b = c->state[1], cc = c->state[2], d = c->state[3];
    uint32_t e = c->state[4], f = c->state[5], g = c->state[6], h = c->state[7];

    for (i = 0; i < 64; i++) {
        uint32_t big1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        uint32_t ch   = (e & f) ^ (~e & g);
        uint32_t t1   = h + big1 + ch + K[i] + W[i];
        uint32_t big0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        uint32_t maj  = (a & b) ^ (a & cc) ^ (b & cc);
        uint32_t t2   = big0 + maj;
        h = g; g = f; f = e; e = d + t1;
        d = cc; cc = b; b = a; a = t1 + t2;
    }

    c->state[0] += a; c->state[1] += b; c->state[2] += cc; c->state[3] += d;
    c->state[4] += e; c->state[5] += f; c->state[6] += g; c->state[7] += h;
}

void glyph_sha256_update(glyph_sha256_ctx *c, const void *data, size_t len)
{
    if (c == NULL || len == 0) {
        return;
    }
    const uint8_t *p = (const uint8_t *)data;
    c->bitlen += (uint64_t)len * 8u;

    while (len > 0) {
        size_t space = 64 - c->datalen;
        size_t take = len < space ? len : space;
        memcpy(c->data + c->datalen, p, take);
        c->datalen += take;
        p += take;
        len -= take;
        if (c->datalen == 64) {
            sha256_transform(c, c->data);
            c->datalen = 0;
        }
    }
}

void glyph_sha256_final(glyph_sha256_ctx *c, uint8_t out[32])
{
    if (c == NULL || out == NULL) {
        return;
    }
    uint64_t bits = c->bitlen;

    /* Append the 0x80 terminator. */
    c->data[c->datalen++] = 0x80;

    /* If padding would cross a block boundary, finish this block first. */
    if (c->datalen > 56) {
        while (c->datalen < 64) {
            c->data[c->datalen++] = 0x00;
        }
        sha256_transform(c, c->data);
        c->datalen = 0;
    }
    /* Zero-pad up to the length field. */
    while (c->datalen < 56) {
        c->data[c->datalen++] = 0x00;
    }
    /* 64-bit big-endian bit length. */
    for (int i = 7; i >= 0; i--) {
        c->data[c->datalen++] = (uint8_t)(bits >> (i * 8));
    }
    sha256_transform(c, c->data);

    /* Emit state, big-endian. */
    for (int i = 0; i < 8; i++) {
        out[i * 4 + 0] = (uint8_t)(c->state[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(c->state[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(c->state[i] >> 8);
        out[i * 4 + 3] = (uint8_t)(c->state[i]);
    }
}

static void bytes_to_hex(const uint8_t digest[32], char hex_out[65])
{
    static const char hc[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        hex_out[i * 2]     = hc[digest[i] >> 4];
        hex_out[i * 2 + 1] = hc[digest[i] & 0x0F];
    }
    hex_out[64] = '\0';
}

int glyph_sha256_buf(const void *data, size_t len, char hex_out[65])
{
    if (data == NULL || hex_out == NULL) {
        errno = EINVAL;
        return -1;
    }
    uint8_t digest[32];
    glyph_sha256_ctx c;
    glyph_sha256_init(&c);
    glyph_sha256_update(&c, data, len);
    glyph_sha256_final(&c, digest);
    bytes_to_hex(digest, hex_out);
    return 0;
}

int glyph_sha256_file(const char *path, char hex_out[65])
{
    if (path == NULL || hex_out == NULL) {
        errno = EINVAL;
        return -1;
    }
    FILE *fp = fopen(path, "rb");
    if (fp == NULL) {
        return -1;
    }

    glyph_sha256_ctx c;
    glyph_sha256_init(&c);
    uint8_t buf[65536];
    size_t got;
    while ((got = fread(buf, 1, sizeof(buf), fp)) > 0) {
        glyph_sha256_update(&c, buf, got);
    }
    int saved_errno = errno;
    if (ferror(fp)) {
        fclose(fp);
        errno = saved_errno;
        return -1;
    }
    fclose(fp);

    uint8_t digest[32];
    glyph_sha256_final(&c, digest);
    bytes_to_hex(digest, hex_out);
    return 0;
}

static int hex_equal_ci(const char *a, const char *b)
{
    while (*a != '\0' && *b != '\0') {
        int ca = tolower((unsigned char)*a);
        int cb = tolower((unsigned char)*b);
        if (ca != cb) {
            return 0;
        }
        a++;
        b++;
    }
    return *a == *b;
}

bool glyph_sha256_verify_file(const char *path, const char *expected_hex)
{
    if (expected_hex == NULL) {
        return false;
    }
    char hex[65];
    if (glyph_sha256_file(path, hex) != 0) {
        return false;
    }
    return hex_equal_ci(hex, expected_hex) != 0;
}

/* ---- Big-integer (2048-bit) + modexp ---- */

/* Little-endian limbs; v[0] is least significant. 64 limbs = 2048 bits. */
typedef struct { uint32_t v[64]; } bn_t;

static void bn_zero(bn_t *x)
{
    for (int i = 0; i < 64; i++) {
        x->v[i] = 0;
    }
}

static void bn_copy(bn_t *dst, const bn_t *src)
{
    memcpy(dst->v, src->v, sizeof(dst->v));
}

static int bn_is_zero(const bn_t *x)
{
    for (int i = 0; i < 64; i++) {
        if (x->v[i] != 0) {
            return 0;
        }
    }
    return 1;
}

/* Big-endian bytes -> little-endian limbs. Keeps the low 256 bytes if longer. */
static void bn_from_be(bn_t *x, const uint8_t *be, size_t len)
{
    bn_zero(x);
    if (len > 256) {
        be += len - 256;
        len = 256;
    }
    for (size_t i = 0; i < len; i++) {
        size_t pos   = len - 1 - i;         /* byte position from LSB */
        size_t limb  = pos >> 2;
        size_t shift = (pos & 3u) * 8u;
        x->v[limb] |= ((uint32_t)be[i]) << shift;
    }
}

static void bn_to_be(const bn_t *x, uint8_t out[256])
{
    for (int i = 0; i < 64; i++) {
        uint32_t limb = x->v[63 - i];
        out[i * 4 + 0] = (uint8_t)(limb >> 24);
        out[i * 4 + 1] = (uint8_t)(limb >> 16);
        out[i * 4 + 2] = (uint8_t)(limb >> 8);
        out[i * 4 + 3] = (uint8_t)(limb);
    }
}

static size_t bn_byte_len(const bn_t *x)
{
    for (int i = 63; i >= 0; i--) {
        if (x->v[i] != 0) {
            uint32_t limb = x->v[i];
            for (int b = 3; b >= 0; b--) {
                if ((limb >> (b * 8)) & 0xFFu) {
                    return (size_t)(i * 4 + b + 1);
                }
            }
        }
    }
    return 0;
}

static int bn_cmp(const bn_t *a, const bn_t *b)
{
    for (int i = 63; i >= 0; i--) {
        if (a->v[i] < b->v[i]) {
            return -1;
        }
        if (a->v[i] > b->v[i]) {
            return 1;
        }
    }
    return 0;
}

/* r = a - b, assuming a >= b. */
static void bn_sub(bn_t *r, const bn_t *a, const bn_t *b)
{
    uint32_t borrow = 0;
    for (int i = 0; i < 64; i++) {
        uint64_t diff = (uint64_t)a->v[i] - (uint64_t)b->v[i] - borrow;
        r->v[i] = (uint32_t)diff;
        borrow = (uint32_t)((diff >> 32) & 1u);
    }
    (void)borrow;
}

/* Schoolbook multiply: out[128] = a * b. */
static void bn_mul(const bn_t *a, const bn_t *b, uint32_t out[128])
{
    for (int i = 0; i < 128; i++) {
        out[i] = 0;
    }
    for (int i = 0; i < 64; i++) {
        uint64_t ai = a->v[i];
        if (ai == 0) {
            continue;
        }
        uint64_t carry = 0;
        for (int j = 0; j < 64; j++) {
            uint64_t cur = (uint64_t)out[i + j] + ai * (uint64_t)b->v[j] + carry;
            out[i + j] = (uint32_t)cur;
            carry = cur >> 32;
        }
        int k = i + 64;
        while (carry && k < 128) {
            uint64_t cur = (uint64_t)out[k] + carry;
            out[k] = (uint32_t)cur;
            carry = cur >> 32;
            k++;
        }
    }
}

/* Reduce a 128-limb product mod a 2048-bit modulus (bit-by-bit long division). */
static void bn_mod(uint32_t prod[128], const bn_t *mod, bn_t *r)
{
    bn_t rem;
    bn_zero(&rem);

    /* Walk dividend bits from MSB (4095) down to LSB (0). */
    for (int i = 4095; i >= 0; i--) {
        /* rem <<= 1 (64 limbs); capture the bit shifted out of bit 2047. */
        uint32_t carry = 0;
        for (int j = 0; j < 64; j++) {
            uint32_t nc = rem.v[j] >> 31;
            rem.v[j] = (rem.v[j] << 1) | carry;
            carry = nc;
        }
        /* Bring in the next dividend bit. */
        rem.v[0] |= (prod[i / 32] >> (i % 32)) & 1u;
        /* True value is (carry << 2048) + rem; subtract mod if >= mod.
         * Invariant keeps the value < 2*mod, so one subtraction suffices
         * and any borrow from bn_sub is absorbed by the (carry << 2048) term. */
        if (carry || bn_cmp(&rem, mod) >= 0) {
            bn_sub(&rem, &rem, mod);
        }
    }
    bn_copy(r, &rem);
}

static void bn_modmul(const bn_t *a, const bn_t *b, const bn_t *mod, bn_t *r)
{
    uint32_t prod[128];
    bn_mul(a, b, prod);
    bn_mod(prod, mod, r);
}

/* Right-to-left square-and-multiply. */
static void bn_modexp(const bn_t *base, const bn_t *exp, const bn_t *mod, bn_t *out)
{
    bn_t result, b;
    bn_zero(&result);
    result.v[0] = 1;  /* result = 1 */

    /* b = base mod mod (base may be >= mod). */
    {
        uint32_t prod[128];
        for (int i = 0; i < 64; i++) {
            prod[i] = base->v[i];
        }
        for (int i = 64; i < 128; i++) {
            prod[i] = 0;
        }
        bn_mod(prod, mod, &b);
    }

    /* Find the top set bit of exp so we only scan what is needed. */
    int top = -1;
    for (int i = 2047; i >= 0; i--) {
        if ((exp->v[i / 32] >> (i % 32)) & 1u) {
            top = i;
            break;
        }
    }

    for (int i = 0; i <= top; i++) {
        if ((exp->v[i / 32] >> (i % 32)) & 1u) {
            bn_modmul(&result, &b, mod, &result);
        }
        if (i < top) {
            bn_modmul(&b, &b, mod, &b);
        }
    }

    bn_copy(out, &result);
}

/* ---- DER parsing for SubjectPublicKeyInfo ---- */

typedef struct { const uint8_t *p; size_t len; } der;

/* Read one TLV; verify the tag; return content pointer/length. */
static int der_read(der *d, uint8_t expected_tag,
                    const uint8_t **content, size_t *clen)
{
    if (d->len < 1) {
        return -1;
    }
    uint8_t tag = d->p[0];
    if (tag != expected_tag) {
        return -1;
    }
    d->p++;
    d->len--;

    if (d->len < 1) {
        return -1;
    }
    uint8_t lb = d->p[0];
    d->p++;
    d->len--;

    size_t len;
    if ((lb & 0x80) == 0) {
        len = lb;
    } else {
        int nbytes = lb & 0x7F;
        if (nbytes == 0 || nbytes > 2) {
            return -1;  /* only short form or 0x81/0x82 long form */
        }
        if (d->len < (size_t)nbytes) {
            return -1;
        }
        len = 0;
        for (int i = 0; i < nbytes; i++) {
            len = (len << 8) | d->p[i];
        }
        d->p += nbytes;
        d->len -= (size_t)nbytes;
    }

    if (d->len < len) {
        return -1;
    }
    *content = d->p;
    *clen = len;
    d->p += len;
    d->len -= len;
    return 0;
}

/* Parse a DER SubjectPublicKeyInfo into modulus n and exponent e. */
static int parse_rsa_pubkey(const uint8_t *buf, size_t len, bn_t *n, bn_t *e)
{
    der top = { buf, len };
    const uint8_t *seq, *alg, *bits;
    size_t seqlen, alglen, bitslen;

    if (der_read(&top, 0x30, &seq, &seqlen) != 0) {
        return -1;
    }

    der d1 = { seq, seqlen };
    /* AlgorithmIdentifier SEQUENCE (contents ignored). */
    if (der_read(&d1, 0x30, &alg, &alglen) != 0) {
        return -1;
    }
    /* subjectPublicKey BIT STRING. */
    if (der_read(&d1, 0x03, &bits, &bitslen) != 0) {
        return -1;
    }
    if (bitslen < 1 || bits[0] != 0x00) {
        return -1;  /* unused bits count must be 0 */
    }

    const uint8_t *rsa = bits + 1;
    size_t rsalen = bitslen - 1;

    /* RSAPublicKey ::= SEQUENCE { modulus INTEGER, publicExponent INTEGER } */
    der d2 = { rsa, rsalen };
    const uint8_t *rsaseq, *mod, *exp;
    size_t rsaseqlen, modlen, explen;
    if (der_read(&d2, 0x30, &rsaseq, &rsaseqlen) != 0) {
        return -1;
    }

    der d3 = { rsaseq, rsaseqlen };
    if (der_read(&d3, 0x02, &mod, &modlen) != 0) {
        return -1;
    }
    if (der_read(&d3, 0x02, &exp, &explen) != 0) {
        return -1;
    }

    /* Strip leading 0x00 sign-padding bytes. */
    while (modlen > 0 && mod[0] == 0x00) {
        mod++;
        modlen--;
    }
    while (explen > 0 && exp[0] == 0x00) {
        exp++;
        explen--;
    }

    if (modlen == 0 || modlen > 256) {
        return -1;
    }
    if (explen == 0 || explen > 256) {
        return -1;
    }

    bn_from_be(n, mod, modlen);
    bn_from_be(e, exp, explen);
    if (bn_is_zero(e)) {
        return -1;
    }
    return 0;
}

/* ---- RSA PKCS#1 v1.5 verify (SHA-256) ---- */

bool glyph_rsa_verify_pkcs1_sha256(const uint8_t *pubkey_der, size_t pubkey_len,
                                   const uint8_t *sig, size_t sig_len,
                                   const uint8_t *data, size_t data_len)
{
    if (pubkey_der == NULL || sig == NULL || data == NULL || sig_len != 256) {
        return false;
    }

    bn_t n, e;
    if (parse_rsa_pubkey(pubkey_der, pubkey_len, &n, &e) != 0) {
        return false;
    }
    if (bn_byte_len(&n) != 256) {
        return false;  /* require a 2048-bit modulus (k = 256) */
    }

    /* s = sig as integer; m = s^e mod n. */
    bn_t s, m;
    bn_from_be(&s, sig, 256);
    bn_modexp(&s, &e, &n, &m);

    /* EM = m as 256 big-endian bytes. */
    uint8_t em[256];
    bn_to_be(&m, em);

    /* SHA-256 of the signed data. */
    uint8_t hash[32];
    glyph_sha256_ctx hc;
    glyph_sha256_init(&hc);
    glyph_sha256_update(&hc, data, data_len);
    glyph_sha256_final(&hc, hash);

    /* DigestInfo prefix for SHA-256 (19 bytes); T = prefix || hash = 51 bytes. */
    static const uint8_t digestinfo[] = {
        0x30, 0x31, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01,
        0x65, 0x03, 0x04, 0x02, 0x01, 0x05, 0x00, 0x04, 0x20
    };

    /* Expected EM = 0x00 || 0x01 || PS(0xFF * 202) || 0x00 || T(51). */
    uint8_t expected[256];
    expected[0] = 0x00;
    expected[1] = 0x01;
    memset(&expected[2], 0xFF, 202);
    expected[204] = 0x00;
    memcpy(&expected[205], digestinfo, 19);
    memcpy(&expected[224], hash, 32);

    /* Constant-time compare. */
    uint8_t acc = 0;
    for (int i = 0; i < 256; i++) {
        acc |= (uint8_t)(em[i] ^ expected[i]);
    }
    return acc == 0;
}

/* ---- Embedded trust anchor ---- */

const uint8_t *glyph_embedded_pubkey_der(size_t *out_len)
{
    if (out_len != NULL) {
        *out_len = GLYPH_PUBKEY_DER_LEN;
    }
    return GLYPH_PUBKEY_DER;
}

int glyph_embedded_pubkey_fingerprint(char hex_out[65])
{
    return glyph_sha256_buf(GLYPH_PUBKEY_DER, GLYPH_PUBKEY_DER_LEN, hex_out);
}

/* Compare a declared fingerprint (any case, optional ':'/' ' separators) with
 * a 64-char lowercase hex string. Returns 1 on exact match. */
static int fp_matches(const char *declared, const char hex[65])
{
    size_t i = 0;
    for (; *declared != '\0'; declared++) {
        if (*declared == ':' || *declared == ' ') {
            continue;
        }
        int c = tolower((unsigned char)*declared);
        if (i >= 64 || c != hex[i]) {
            return 0;
        }
        i++;
    }
    return i == 64;
}

int glyph_verify_catalog_signature(const char *cat_bytes, size_t cat_len,
                                   const uint8_t *sig, size_t sig_len,
                                   const char *declared_fp)
{
    if (cat_bytes == NULL || sig == NULL) {
        errno = EINVAL;
        return -1;
    }

    double t0 = glyph_now_sec();

    char fp[65];
    if (glyph_embedded_pubkey_fingerprint(fp) != 0) {
        return -1;
    }

    /* Diagnostic first: a fingerprint mismatch means the signing key rotated
     * without a binary update -- say so instead of failing opaquely. */
    if (declared_fp == NULL || !fp_matches(declared_fp, fp)) {
        glyph_log_err(
            "catalog declares signature fingerprint %s, but this binary "
            "carries %s -- the signing key rotated without a binary update",
            declared_fp != NULL ? declared_fp : "(none)", fp);
        glyph_log_debug("verify: fp-mismatch elapsed=%.3fs", glyph_now_sec() - t0);
        return -2;
    }

    if (!glyph_rsa_verify_pkcs1_sha256(GLYPH_PUBKEY_DER, GLYPH_PUBKEY_DER_LEN,
                                       sig, sig_len,
                                       (const uint8_t *)cat_bytes, cat_len)) {
        glyph_log_err("catalog signature verification failed");
        glyph_log_debug("verify: failed elapsed=%.3fs fp=%.16s...", glyph_now_sec() - t0, fp);
        return -1;
    }
    glyph_log_debug("verify: ok elapsed=%.3fs fp=%.16s...", glyph_now_sec() - t0, fp);
    return 0;
}
