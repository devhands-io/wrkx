#ifndef PG_SCRAM_H
#define PG_SCRAM_H

/*
 * SCRAM-SHA-256 client implementation (RFC 5802 / RFC 7677).
 * No networking, no wrkx engine headers.
 *
 * ADR 0005, Phase 6 (P6-3).
 */

#include <stddef.h>
#include <stdint.h>

/*
 * Generate the client-first-message-bare: "n=<escaped_username>,r=<nonce>".
 * The GS2 header ("n,,") is NOT included here; the caller prepends it when
 * building the SASLInitialResponse payload.
 * nonce_out receives the bare nonce token (for pg_scram_client_final).
 * Returns bytes written into buf (>= 0), or -1 on error.
 */
int pg_scram_client_first(const char *username,
                          char nonce_out[64],
                          char *buf, size_t cap);

/*
 * Compute the client-final-message.
 *
 * server_first / sf_len:   data from PG_MSG_AUTH_SASL_CONTINUE.
 * client_nonce:            nonce from pg_scram_client_first.
 * client_first_bare / cf_bare_len: buf from pg_scram_client_first.
 * password:                cleartext password.
 * expected_server_sig_out: receives HMAC(ServerKey, AuthMessage) — 32 bytes.
 *                          Pass to pg_scram_verify_server.
 * buf / cap:               output buffer for the client-final-message.
 *
 * Returns bytes written into buf, or -1 on error.
 */
int pg_scram_client_final(const char *server_first, size_t sf_len,
                          const char *client_nonce,
                          const char *client_first_bare, size_t cf_bare_len,
                          const char *password,
                          uint8_t expected_server_sig_out[32],
                          char *buf, size_t cap);

/*
 * Verify the server-final-message.
 * expected_server_sig: 32-byte HMAC(ServerKey, AuthMessage) from client_final.
 * Returns 1 if the signature matches, 0 otherwise.
 */
int pg_scram_verify_server(const char *server_final, size_t sf_len,
                           const uint8_t expected_server_sig[32]);

#endif /* PG_SCRAM_H */
