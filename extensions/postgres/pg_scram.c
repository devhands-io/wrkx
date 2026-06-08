/*
 * pg_scram.c — SCRAM-SHA-256 client (RFC 5802 / RFC 7677).
 *
 * ADR 0005, Phase 6 (P6-3).
 * Uses OpenSSL primitives already linked by the build; no new dependencies.
 */

#include "pg_scram.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>

/* -------------------------------------------------------------------------
 * Internal crypto helpers
 * ---------------------------------------------------------------------- */

/* base64-encode `inlen` bytes into `out` (NUL-terminated).
 * Returns encoded length (excluding NUL), or -1 if `outcap` is too small. */
static int b64_encode(const uint8_t *in, size_t inlen,
                      char *out, size_t outcap) {
    int n = EVP_EncodeBlock((unsigned char *)out, in, (int)inlen);
    if (n < 0 || (size_t)n >= outcap) return -1;
    out[n] = '\0';
    return n;
}

/* base64-decode NUL-terminated `in` into `out`.
 * Returns decoded byte count, or -1 on error. */
static int b64_decode(const char *in, uint8_t *out, size_t outcap) {
    size_t inlen = strlen(in);
    if (inlen == 0 || inlen % 4 != 0) return -1;

    int n = EVP_DecodeBlock(out, (const unsigned char *)in, (int)inlen);
    if (n < 0) return -1;

    /* subtract padding bytes */
    if (inlen >= 1 && in[inlen - 1] == '=') n--;
    if (inlen >= 2 && in[inlen - 2] == '=') n--;

    if (n < 0 || (size_t)n > outcap) return -1;
    return n;
}

static void hmac_sha256(const uint8_t *key, size_t keylen,
                        const uint8_t *data, size_t datalen,
                        uint8_t out[32]) {
    unsigned int mdlen = 32;
    HMAC(EVP_sha256(), key, (int)keylen, data, datalen, out, &mdlen);
}

static void sha256(const uint8_t *data, size_t datalen, uint8_t out[32]) {
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    unsigned int mdlen = 32;
    EVP_DigestInit_ex(ctx, EVP_sha256(), NULL);
    EVP_DigestUpdate(ctx, data, datalen);
    EVP_DigestFinal_ex(ctx, out, &mdlen);
    EVP_MD_CTX_free(ctx);
}

/* -------------------------------------------------------------------------
 * pg_scram_client_first
 * ---------------------------------------------------------------------- */

int pg_scram_client_first(const char *username,
                          char nonce_out[64],
                          char *buf, size_t cap) {
    /* Generate 18 random bytes → 24-char base64 nonce (no commas) */
    uint8_t rand_bytes[18];
    if (!RAND_bytes(rand_bytes, (int)sizeof(rand_bytes))) return -1;

    char nonce[32];
    if (b64_encode(rand_bytes, sizeof(rand_bytes), nonce, sizeof(nonce)) < 0)
        return -1;

    size_t nonce_len = strlen(nonce);
    if (nonce_len >= 64) return -1;
    memcpy(nonce_out, nonce, nonce_len + 1);

    /* Escape username per RFC 5802 §5.1: ',' → "=2C", '=' → "=3D" */
    char esc[512];
    size_t ep = 0;
    for (const char *p = username; *p; p++) {
        if (*p == ',') {
            if (ep + 3 > sizeof(esc) - 1) return -1;
            memcpy(esc + ep, "=2C", 3); ep += 3;
        } else if (*p == '=') {
            if (ep + 3 > sizeof(esc) - 1) return -1;
            memcpy(esc + ep, "=3D", 3); ep += 3;
        } else {
            if (ep + 1 > sizeof(esc) - 1) return -1;
            esc[ep++] = *p;
        }
    }
    esc[ep] = '\0';

    int n = snprintf(buf, cap, "n=%s,r=%s", esc, nonce);
    if (n <= 0 || (size_t)n >= cap) return -1;
    return n;
}

/* -------------------------------------------------------------------------
 * pg_scram_client_final
 * ---------------------------------------------------------------------- */

int pg_scram_client_final(const char *server_first, size_t sf_len,
                          const char *client_nonce,
                          const char *client_first_bare, size_t cf_bare_len,
                          const char *password,
                          uint8_t expected_server_sig_out[32],
                          char *buf, size_t cap) {
    /* Copy server-first into a NUL-terminated buffer for parsing */
    char sf[512];
    if (sf_len >= sizeof(sf)) return -1;
    memcpy(sf, server_first, sf_len);
    sf[sf_len] = '\0';

    /* Parse r= (combined nonce) */
    const char *r_tok = strstr(sf, "r=");
    if (!r_tok) return -1;
    r_tok += 2;
    const char *r_end = strchr(r_tok, ',');
    size_t r_len = r_end ? (size_t)(r_end - r_tok) : strlen(r_tok);
    char combined_nonce[256];
    if (r_len >= sizeof(combined_nonce)) return -1;
    memcpy(combined_nonce, r_tok, r_len);
    combined_nonce[r_len] = '\0';

    /* Combined nonce must start with the client nonce */
    size_t cn_len = strlen(client_nonce);
    if (r_len < cn_len || memcmp(combined_nonce, client_nonce, cn_len) != 0)
        return -1;

    /* Parse s= (base64-encoded salt) */
    const char *s_tok = strstr(sf, "s=");
    if (!s_tok) return -1;
    s_tok += 2;
    const char *s_end = strchr(s_tok, ',');
    size_t s_len = s_end ? (size_t)(s_end - s_tok) : strlen(s_tok);
    char s_b64[128];
    if (s_len >= sizeof(s_b64)) return -1;
    memcpy(s_b64, s_tok, s_len);
    s_b64[s_len] = '\0';

    uint8_t salt[64];
    int salt_len = b64_decode(s_b64, salt, sizeof(salt));
    if (salt_len < 0) return -1;

    /* Parse i= (iteration count) */
    const char *i_tok = strstr(sf, "i=");
    if (!i_tok) return -1;
    i_tok += 2;
    int iters = atoi(i_tok);
    if (iters <= 0) return -1;

    /* SaltedPassword = PBKDF2-HMAC-SHA256(password, salt, iters, 32) */
    uint8_t salted_password[32];
    if (!PKCS5_PBKDF2_HMAC(password, (int)strlen(password),
                            salt, salt_len, iters,
                            EVP_sha256(), 32, salted_password))
        return -1;

    /* ClientKey = HMAC(SaltedPassword, "Client Key") */
    uint8_t client_key[32];
    hmac_sha256(salted_password, 32,
                (const uint8_t *)"Client Key", 10, client_key);

    /* StoredKey = SHA256(ClientKey) */
    uint8_t stored_key[32];
    sha256(client_key, 32, stored_key);

    /* client-final-without-proof = "c=biws,r=<combined_nonce>"
     * "biws" = base64("n,,") */
    char cfm_noproof[512];
    int cfm_len = snprintf(cfm_noproof, sizeof(cfm_noproof),
                           "c=biws,r=%s", combined_nonce);
    if (cfm_len <= 0 || (size_t)cfm_len >= sizeof(cfm_noproof)) return -1;

    /* AuthMessage = client-first-bare "," server-first "," cfm-noproof */
    char auth_msg[2048];
    int am_len = snprintf(auth_msg, sizeof(auth_msg),
                          "%.*s,%.*s,%s",
                          (int)cf_bare_len, client_first_bare,
                          (int)sf_len, server_first,
                          cfm_noproof);
    if (am_len <= 0 || (size_t)am_len >= sizeof(auth_msg)) return -1;

    /* ClientSignature = HMAC(StoredKey, AuthMessage) */
    uint8_t client_sig[32];
    hmac_sha256(stored_key, 32,
                (const uint8_t *)auth_msg, (size_t)am_len, client_sig);

    /* ClientProof = ClientKey XOR ClientSignature */
    uint8_t client_proof[32];
    for (int i = 0; i < 32; i++)
        client_proof[i] = client_key[i] ^ client_sig[i];

    /* ServerKey = HMAC(SaltedPassword, "Server Key") */
    uint8_t server_key[32];
    hmac_sha256(salted_password, 32,
                (const uint8_t *)"Server Key", 10, server_key);

    /* ServerSignature = HMAC(ServerKey, AuthMessage) — returned to caller */
    hmac_sha256(server_key, 32,
                (const uint8_t *)auth_msg, (size_t)am_len,
                expected_server_sig_out);

    /* base64-encode the client proof */
    char proof_b64[64];
    if (b64_encode(client_proof, 32, proof_b64, sizeof(proof_b64)) < 0)
        return -1;

    /* client-final = cfm-noproof + ",p=" + proof */
    int n = snprintf(buf, cap, "%s,p=%s", cfm_noproof, proof_b64);
    if (n <= 0 || (size_t)n >= cap) return -1;
    return n;
}

/* -------------------------------------------------------------------------
 * pg_scram_verify_server
 * ---------------------------------------------------------------------- */

int pg_scram_verify_server(const char *server_final, size_t sf_len,
                           const uint8_t expected_server_sig[32]) {
    char sf[512];
    if (sf_len >= sizeof(sf)) return 0;
    memcpy(sf, server_final, sf_len);
    sf[sf_len] = '\0';

    /* Find "v=" field */
    const char *v = strstr(sf, "v=");
    if (!v) return 0;
    v += 2;

    uint8_t decoded[64];
    int dlen = b64_decode(v, decoded, sizeof(decoded));
    if (dlen != 32) return 0;

    return (memcmp(decoded, expected_server_sig, 32) == 0) ? 1 : 0;
}
