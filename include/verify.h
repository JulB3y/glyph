#ifndef GLYPH_VERIFY_H
#define GLYPH_VERIFY_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* ---- SHA-256 (RFC 6234) ---- */
typedef struct {
    uint32_t state[8];
    uint64_t bitlen;
    uint8_t  data[64];
    size_t   datalen;
} glyph_sha256_ctx;

void glyph_sha256_init(glyph_sha256_ctx *c);
void glyph_sha256_update(glyph_sha256_ctx *c, const void *data, size_t len);
void glyph_sha256_final(glyph_sha256_ctx *c, uint8_t out[32]);

/* hex_out must be >=65 bytes; lowercase hex + NUL. */
int glyph_sha256_buf(const void *data, size_t len, char hex_out[65]);
int glyph_sha256_file(const char *path, char hex_out[65]);

/* Case-insensitive compare of a file's sha256 to expected hex. */
bool glyph_sha256_verify_file(const char *path, const char *expected_hex);

/* ---- RSA-2048 PKCS#1 v1.5 verify (SHA-256 digest) ----
   Implements verification ONLY (no key generation / signing).
   pubkey_der: DER SubjectPublicKeyInfo (or raw RSAPublicKey) for a 2048-bit key.
   Returns true iff signature is valid for data. */
bool glyph_rsa_verify_pkcs1_sha256(const uint8_t *pubkey_der, size_t pubkey_len,
                                   const uint8_t *sig, size_t sig_len,
                                   const uint8_t *data, size_t data_len);

/* ---- Embedded trust anchor (build-time DER from res/glyph.pem) ----
   There is no TOFU store and no runtime key loading: the ONLY verifying key
   is the DER SubjectPublicKeyInfo embedded at build time. */

/* Returns the embedded pubkey DER bytes (static storage, do not free) and
   sets *out_len (if non-NULL) to the byte count. */
const uint8_t *glyph_embedded_pubkey_der(size_t *out_len);

/* SHA-256 fingerprint of the embedded pubkey DER as 64-char lowercase hex.
   hex_out must be >= 65 bytes. Returns 0 on success. */
int glyph_embedded_pubkey_fingerprint(char hex_out[65]);

/* Verify catalog bytes + signature strictly against the embedded key.
   If declared_fp is non-NULL it is compared (case-insensitive, ':' and ' '
   separators ignored) against the embedded key's fingerprint first; on
   mismatch logs a key-rotation diagnostic and returns -2. Signature failure
   (or bad inputs) returns -1. Returns 0 iff the catalog is authentic. */
int glyph_verify_catalog_signature(const char *cat_bytes, size_t cat_len,
                                   const uint8_t *sig, size_t sig_len,
                                   const char *declared_fp);

#endif /* GLYPH_VERIFY_H */
