/*
 * tests/unit/test_pg_scram.c
 *
 * Unit tests for SCRAM-SHA-256 (RFC 7677) client implementation.
 * Links only pg_scram.c + OpenSSL; no LuaJIT, no protocol code.
 *
 * ADR 0005, P6-3.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "unity.h"
#include "pg_scram.h"

void setUp(void)    {}
void tearDown(void) {}

/* =========================================================================
 * Helper
 * ====================================================================== */


/* =========================================================================
 * Tests
 * ====================================================================== */

void test_client_first_generates_nonce(void) {
    char nonce[64] = {0};
    char buf[256]  = {0};
    int n = pg_scram_client_first("alice", nonce, buf, sizeof(buf));
    TEST_ASSERT_GREATER_THAN_INT(0, n);
    /* bare message must start with "n=alice,r=" */
    TEST_ASSERT_TRUE(strncmp(buf, "n=alice,r=", 10) == 0);
    /* nonce must be at least 16 chars */
    TEST_ASSERT_GREATER_OR_EQUAL(16, (int)strlen(nonce));
    /* nonce must not contain a comma */
    TEST_ASSERT_NULL(strchr(nonce, ','));
}

void test_client_first_username_escaping(void) {
    char nonce[64] = {0};
    char buf[256]  = {0};
    int n = pg_scram_client_first("a,b=c", nonce, buf, sizeof(buf));
    TEST_ASSERT_GREATER_THAN_INT(0, n);
    /* ',' → "=2C", '=' → "=3D" in the n= attribute */
    TEST_ASSERT_TRUE(strncmp(buf, "n=a=2Cb=3Dc,r=", 14) == 0);
}

void test_client_final_known_vector(void) {
    /*
     * RFC 7677 SCRAM-SHA-256 test vector.
     * user="user", password="pencil"
     * client nonce: "rOprNGfwEbeRWgbNEkqO"
     * server-first: "r=rOprNGfwEbeRWgbNEkqO%hvYDpWUa2RaTCAfuxFIlj)hNlF$k0,
     *                s=W22ZaJ0SNY7soEsUEjb6gQ==,i=4096"
     */
    const char *client_nonce   = "rOprNGfwEbeRWgbNEkqO";
    const char *client_first_bare =
        "n=user,r=rOprNGfwEbeRWgbNEkqO";
    const char *server_first   =
        "r=rOprNGfwEbeRWgbNEkqO%hvYDpWUa2RaTCAfuxFIlj)hNlF$k0,"
        "s=W22ZaJ0SNY7soEsUEjb6gQ==,i=4096";

    uint8_t exp_sig[32];
    char buf[512];
    int n = pg_scram_client_final(
        server_first, strlen(server_first),
        client_nonce,
        client_first_bare, strlen(client_first_bare),
        "pencil",
        exp_sig,
        buf, sizeof(buf));

    TEST_ASSERT_GREATER_THAN_INT(0, n);
    /* client-final must start with "c=biws,r=rOprNGfwEbeRWgbNEkqO" (29 chars) */
    TEST_ASSERT_TRUE(strncmp(buf, "c=biws,r=rOprNGfwEbeRWgbNEkqO", 29) == 0);
    /* must contain ",p=" */
    TEST_ASSERT_NOT_NULL(strstr(buf, ",p="));
    /* RFC 7677 client-proof — note: RFC document has a typo ('9'); correct
     * value derived independently via Python hmac + PBKDF2 is '7'. */
    const char *expected_proof =
        "dHzbZapWIk4jUhN+Ute9ytag9zjfMHgsqmmiz7AndVQ=";
    const char *p_field = strstr(buf, ",p=");
    TEST_ASSERT_NOT_NULL(p_field);
    TEST_ASSERT_EQUAL_STRING(expected_proof, p_field + 3);
}

void test_server_verify_correct_signature(void) {
    /* RFC 7677 server-final: v=6rriTRBi23WpRR/wtup+mMhUZUn/dB5nLTJRsjl95G4= */
    const char *client_nonce      = "rOprNGfwEbeRWgbNEkqO";
    const char *client_first_bare = "n=user,r=rOprNGfwEbeRWgbNEkqO";
    const char *server_first =
        "r=rOprNGfwEbeRWgbNEkqO%hvYDpWUa2RaTCAfuxFIlj)hNlF$k0,"
        "s=W22ZaJ0SNY7soEsUEjb6gQ==,i=4096";
    const char *server_final =
        "v=6rriTRBi23WpRR/wtup+mMhUZUn/dB5nLTJRsjl95G4=";

    uint8_t exp_sig[32];
    char buf[512];
    int n = pg_scram_client_final(
        server_first, strlen(server_first),
        client_nonce,
        client_first_bare, strlen(client_first_bare),
        "pencil", exp_sig, buf, sizeof(buf));
    TEST_ASSERT_GREATER_THAN_INT(0, n);

    int ok = pg_scram_verify_server(server_final, strlen(server_final),
                                    exp_sig);
    TEST_ASSERT_EQUAL_INT(1, ok);
}

void test_server_verify_wrong_signature(void) {
    const char *client_nonce      = "rOprNGfwEbeRWgbNEkqO";
    const char *client_first_bare = "n=user,r=rOprNGfwEbeRWgbNEkqO";
    const char *server_first =
        "r=rOprNGfwEbeRWgbNEkqO%hvYDpWUa2RaTCAfuxFIlj)hNlF$k0,"
        "s=W22ZaJ0SNY7soEsUEjb6gQ==,i=4096";
    const char *server_final =
        "v=6rriTRBi23WpRR/wtup+mMhUZUn/dB5nLTJRsjl95G4=";

    uint8_t exp_sig[32];
    char buf[512];
    int n = pg_scram_client_final(
        server_first, strlen(server_first),
        client_nonce,
        client_first_bare, strlen(client_first_bare),
        "pencil", exp_sig, buf, sizeof(buf));
    TEST_ASSERT_GREATER_THAN_INT(0, n);

    /* corrupt one byte */
    exp_sig[0] ^= 0xff;
    int ok = pg_scram_verify_server(server_final, strlen(server_final),
                                    exp_sig);
    TEST_ASSERT_EQUAL_INT(0, ok);
}

void test_client_final_rejects_wrong_nonce(void) {
    /* server-first with a combined nonce that does NOT start with client nonce */
    const char *server_first =
        "r=DIFFERENTNONCE,s=W22ZaJ0SNY7soEsUEjb6gQ==,i=4096";
    uint8_t exp_sig[32];
    char buf[512];
    int n = pg_scram_client_final(
        server_first, strlen(server_first),
        "rOprNGfwEbeRWgbNEkqO",
        "n=user,r=rOprNGfwEbeRWgbNEkqO",
        strlen("n=user,r=rOprNGfwEbeRWgbNEkqO"),
        "pencil", exp_sig, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_INT(-1, n);
}

void test_client_final_rejects_missing_s_field(void) {
    const char *server_first =
        "r=rOprNGfwEbeRWgbNEkqO%hvYDpWUa2RaTCAfuxFIlj)hNlF$k0,i=4096";
    uint8_t exp_sig[32];
    char buf[512];
    int n = pg_scram_client_final(
        server_first, strlen(server_first),
        "rOprNGfwEbeRWgbNEkqO",
        "n=user,r=rOprNGfwEbeRWgbNEkqO",
        strlen("n=user,r=rOprNGfwEbeRWgbNEkqO"),
        "pencil", exp_sig, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_INT(-1, n);
}

void test_client_final_rejects_zero_iterations(void) {
    const char *server_first =
        "r=rOprNGfwEbeRWgbNEkqO%hvYDpWUa2RaTCAfuxFIlj)hNlF$k0,"
        "s=W22ZaJ0SNY7soEsUEjb6gQ==,i=0";
    uint8_t exp_sig[32];
    char buf[512];
    int n = pg_scram_client_final(
        server_first, strlen(server_first),
        "rOprNGfwEbeRWgbNEkqO",
        "n=user,r=rOprNGfwEbeRWgbNEkqO",
        strlen("n=user,r=rOprNGfwEbeRWgbNEkqO"),
        "pencil", exp_sig, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_INT(-1, n);
}

/* =========================================================================
 * main
 * ====================================================================== */

int main(void) {
    UNITY_BEGIN();

    RUN_TEST(test_client_first_generates_nonce);
    RUN_TEST(test_client_first_username_escaping);
    RUN_TEST(test_client_final_known_vector);
    RUN_TEST(test_server_verify_correct_signature);
    RUN_TEST(test_server_verify_wrong_signature);
    RUN_TEST(test_client_final_rejects_wrong_nonce);
    RUN_TEST(test_client_final_rejects_missing_s_field);
    RUN_TEST(test_client_final_rejects_zero_iterations);

    return UNITY_END();
}
