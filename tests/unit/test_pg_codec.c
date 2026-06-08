/*
 * tests/unit/test_pg_codec.c
 *
 * Unit tests for the PostgreSQL wire-protocol codec (ADR 0005, P6-1 + P6-2).
 *
 * Tests pg_message.c in isolation: no TCP, no engine, no wrkx core.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "unity.h"
#include "pg_message.h"

void setUp(void)    {}
void tearDown(void) {}

/* =========================================================================
 * Big-endian read helpers (local — codec keeps helpers static)
 * ====================================================================== */

static int32_t rd_i32(const char *p) {
    const unsigned char *u = (const unsigned char *)p;
    return (int32_t)(((uint32_t)u[0] << 24) | ((uint32_t)u[1] << 16) |
                     ((uint32_t)u[2] <<  8) |  (uint32_t)u[3]);
}
static int16_t rd_i16(const char *p) {
    const unsigned char *u = (const unsigned char *)p;
    return (int16_t)(((uint16_t)u[0] << 8) | (uint16_t)u[1]);
}

/* =========================================================================
 * P6-1 encoder tests
 * ====================================================================== */

void test_encode_startup_format(void) {
    char buf[256];
    int n = pg_encode_startup(buf, sizeof(buf), "alice", "mydb");
    TEST_ASSERT_GREATER_THAN_INT(0, n);
    /* total length stored in first int32 must equal n */
    TEST_ASSERT_EQUAL_INT(n, (int)rd_i32(buf));
    /* protocol version 3.0 */
    TEST_ASSERT_EQUAL_INT32(0x00030000, rd_i32(buf + 4));
    /* "user\0alice\0database\0mydb\0\0" starting at offset 8 */
    TEST_ASSERT_EQUAL_MEMORY("user", buf + 8, 4);
    TEST_ASSERT_EQUAL_CHAR('\0', buf[12]);
    TEST_ASSERT_EQUAL_MEMORY("alice", buf + 13, 5);
    TEST_ASSERT_EQUAL_CHAR('\0', buf[18]);
    TEST_ASSERT_EQUAL_MEMORY("database", buf + 19, 8);
    TEST_ASSERT_EQUAL_CHAR('\0', buf[27]);
    TEST_ASSERT_EQUAL_MEMORY("mydb", buf + 28, 4);
    TEST_ASSERT_EQUAL_CHAR('\0', buf[32]);
    TEST_ASSERT_EQUAL_CHAR('\0', buf[33]); /* terminating \0 */
}

void test_encode_query_format(void) {
    char buf[64];
    const char *sql = "SELECT 1";
    int n = pg_encode_query(buf, sizeof(buf), sql);
    TEST_ASSERT_EQUAL_INT(1 + 4 + 8 + 1, n); /* 'Q' + len + sql + \0 */
    TEST_ASSERT_EQUAL_CHAR('Q', buf[0]);
    TEST_ASSERT_EQUAL_INT32((int32_t)(4 + 8 + 1), rd_i32(buf + 1));
    TEST_ASSERT_EQUAL_MEMORY(sql, buf + 5, 8);
    TEST_ASSERT_EQUAL_CHAR('\0', buf[13]);
}

void test_encode_password_cleartext(void) {
    char buf[64];
    const char *pw = "hunter2";
    int n = pg_encode_password(buf, sizeof(buf), pw);
    int expected = 1 + 4 + 7 + 1;
    TEST_ASSERT_EQUAL_INT(expected, n);
    TEST_ASSERT_EQUAL_CHAR('p', buf[0]);
    TEST_ASSERT_EQUAL_INT32((int32_t)(4 + 7 + 1), rd_i32(buf + 1));
    TEST_ASSERT_EQUAL_MEMORY(pw, buf + 5, 7);
    TEST_ASSERT_EQUAL_CHAR('\0', buf[12]);
}

void test_encode_md5_password(void) {
    /* Known vector: user="wrkx" password="secret" salt={1,2,3,4}
     * inner = MD5("secretwrkx") = ad0d411b26b9b9f12f03990dd3cade93
     * outer = MD5(inner_hex + salt) = b69d683b91a95796a26e7686019ef9fa
     * wire password = "md5b69d683b91a95796a26e7686019ef9fa"  */
    char buf[128];
    uint8_t salt[4] = {1, 2, 3, 4};
    int n = pg_encode_md5_password(buf, sizeof(buf), "secret", "wrkx", salt);
    TEST_ASSERT_GREATER_THAN_INT(0, n);
    TEST_ASSERT_EQUAL_CHAR('p', buf[0]);
    /* password string at buf+5: "md5" + 32 hex chars + '\0' */
    TEST_ASSERT_EQUAL_CHAR('m', buf[5]);
    TEST_ASSERT_EQUAL_CHAR('d', buf[6]);
    TEST_ASSERT_EQUAL_CHAR('5', buf[7]);
    TEST_ASSERT_EQUAL_MEMORY("b69d683b91a95796a26e7686019ef9fa",
                             buf + 8, 32);
    TEST_ASSERT_EQUAL_CHAR('\0', buf[40]);
}

/* =========================================================================
 * P6-1 parser tests
 * ====================================================================== */

void test_parse_auth_ok(void) {
    /* 'R' + int32(8) + int32(0) = 9 bytes */
    const char msg[] = "\x52\x00\x00\x00\x08\x00\x00\x00\x00";
    pg_parsed_msg out;
    int rc = pg_parse_message(msg, 9, &out);
    TEST_ASSERT_EQUAL_INT(9, rc);
    TEST_ASSERT_EQUAL_INT(PG_MSG_AUTH_OK, (int)out.type);
}

void test_parse_auth_cleartext(void) {
    const char msg[] = "\x52\x00\x00\x00\x08\x00\x00\x00\x03";
    pg_parsed_msg out;
    int rc = pg_parse_message(msg, 9, &out);
    TEST_ASSERT_EQUAL_INT(9, rc);
    TEST_ASSERT_EQUAL_INT(PG_MSG_AUTH_CLEARTEXT, (int)out.type);
}

void test_parse_auth_md5(void) {
    /* 'R' + int32(12) + int32(5) + salt{0x01,0x02,0x03,0x04} */
    const char msg[] = "\x52\x00\x00\x00\x0c\x00\x00\x00\x05\x01\x02\x03\x04";
    pg_parsed_msg out;
    int rc = pg_parse_message(msg, 13, &out);
    TEST_ASSERT_EQUAL_INT(13, rc);
    TEST_ASSERT_EQUAL_INT(PG_MSG_AUTH_MD5, (int)out.type);
    TEST_ASSERT_EQUAL_UINT8(0x01, out.md5.salt[0]);
    TEST_ASSERT_EQUAL_UINT8(0x02, out.md5.salt[1]);
    TEST_ASSERT_EQUAL_UINT8(0x03, out.md5.salt[2]);
    TEST_ASSERT_EQUAL_UINT8(0x04, out.md5.salt[3]);
}

void test_parse_auth_sasl(void) {
    /* 'R' + int32(len) + int32(10) + "SCRAM-SHA-256\0\0" */
    /* body = int32(10) + "SCRAM-SHA-256\0\0" = 4 + 13 + 1 + 1 = 19 */
    /* msglen = 4 + 19 = 23; total = 24 */
    char msg[32];
    msg[0] = 'R';
    msg[1] = 0; msg[2] = 0; msg[3] = 0; msg[4] = 23;   /* int32(23) */
    msg[5] = 0; msg[6] = 0; msg[7] = 0; msg[8] = 10;   /* int32(10) */
    memcpy(msg + 9, "SCRAM-SHA-256", 13);
    msg[22] = '\0';   /* mechanism terminator */
    msg[23] = '\0';   /* list terminator */
    pg_parsed_msg out;
    int rc = pg_parse_message(msg, 24, &out);
    TEST_ASSERT_EQUAL_INT(24, rc);
    TEST_ASSERT_EQUAL_INT(PG_MSG_AUTH_SASL, (int)out.type);
    TEST_ASSERT_EQUAL_MEMORY("SCRAM-SHA-256", out.sasl.sasl_mechanisms, 13);
}

void test_parse_ready_for_query(void) {
    /* 'Z' + int32(5) + 'I' */
    const char msg[] = "\x5a\x00\x00\x00\x05\x49";
    pg_parsed_msg out;
    int rc = pg_parse_message(msg, 6, &out);
    TEST_ASSERT_EQUAL_INT(6, rc);
    TEST_ASSERT_EQUAL_INT(PG_MSG_READY_FOR_QUERY, (int)out.type);
}

void test_parse_command_complete(void) {
    /* 'C' + int32(13) + "SELECT 1\0" (msglen = 4+9 = 13) */
    char msg[16];
    msg[0] = 'C';
    msg[1] = 0; msg[2] = 0; msg[3] = 0; msg[4] = 13;
    memcpy(msg + 5, "SELECT 1\0", 9);
    pg_parsed_msg out;
    int rc = pg_parse_message(msg, 14, &out);
    TEST_ASSERT_EQUAL_INT(14, rc);
    TEST_ASSERT_EQUAL_INT(PG_MSG_COMMAND_COMPLETE, (int)out.type);
    TEST_ASSERT_EQUAL_MEMORY("SELECT 1", out.cmd_complete.tag, 8);
}

void test_parse_error_response(void) {
    /* 'E' + int32(len) + 'S' + "ERROR\0" + 'M' + "bad input\0" + '\0' */
    const char *sev = "ERROR";
    const char *msg_text = "bad input";
    size_t body_len = 1 + 5 + 1 + 1 + 9 + 1 + 1; /* S+sev+\0 + M+msg+\0 + \0 */
    int32_t msglen = (int32_t)(4 + body_len);

    char buf[64];
    buf[0] = 'E';
    buf[1] = (char)((msglen >> 24) & 0xff);
    buf[2] = (char)((msglen >> 16) & 0xff);
    buf[3] = (char)((msglen >>  8) & 0xff);
    buf[4] = (char)( msglen        & 0xff);
    char *p = buf + 5;
    *p++ = 'S';
    memcpy(p, sev, 5); p += 5; *p++ = '\0';
    *p++ = 'M';
    memcpy(p, msg_text, 9); p += 9; *p++ = '\0';
    *p++ = '\0';
    size_t total = (size_t)(1 + msglen);

    pg_parsed_msg out;
    int rc = pg_parse_message(buf, total, &out);
    TEST_ASSERT_EQUAL_INT((int)total, rc);
    TEST_ASSERT_EQUAL_INT(PG_MSG_ERROR_RESPONSE, (int)out.type);
    TEST_ASSERT_EQUAL_MEMORY(sev, out.error.severity, 5);
    TEST_ASSERT_EQUAL_MEMORY(msg_text, out.error.message, 9);
}

void test_parse_data_row(void) {
    /* 'D' + int32(10) + int16(1) + int32(-1)  — one null field */
    char msg[12];
    msg[0] = 'D';
    msg[1] = 0; msg[2] = 0; msg[3] = 0; msg[4] = 10;  /* msglen=10 */
    msg[5] = 0; msg[6] = 1;                            /* int16(1) nfields */
    msg[7] = 0xff; msg[8] = 0xff; msg[9] = 0xff; msg[10] = 0xff; /* int32(-1) */
    pg_parsed_msg out;
    int rc = pg_parse_message(msg, 11, &out);
    TEST_ASSERT_EQUAL_INT(11, rc);
    TEST_ASSERT_EQUAL_INT(PG_MSG_DATA_ROW, (int)out.type);
    TEST_ASSERT_EQUAL_INT16(1, out.data_row.nfields);
}

/* Build a RowDescription wire message with ncols columns, each named "cx" */
static void build_row_description(char *buf, size_t cap,
                                  int ncols, size_t *out_total) {
    /* per-column: "cx\0" (3 bytes) + 18 fixed bytes = 21 bytes */
    size_t per_col  = 3 + 18;
    size_t body_len = 2 + (size_t)ncols * per_col;
    int32_t msglen  = (int32_t)(4 + body_len);
    *out_total = 1 + (size_t)msglen;
    if (*out_total > cap) { *out_total = 0; return; }

    buf[0] = 'T';
    buf[1] = (char)((msglen >> 24) & 0xff);
    buf[2] = (char)((msglen >> 16) & 0xff);
    buf[3] = (char)((msglen >>  8) & 0xff);
    buf[4] = (char)( msglen        & 0xff);
    buf[5] = (char)((ncols >> 8) & 0xff);
    buf[6] = (char)( ncols       & 0xff);
    char *p = buf + 7;
    for (int i = 0; i < ncols; i++) {
        *p++ = 'c'; *p++ = 'x'; *p++ = '\0';
        memset(p, 0, 18); p += 18;
    }
}

void test_parse_row_description(void) {
    /* 3 columns each named "cx" */
    char msg[256];
    size_t total = 0;
    build_row_description(msg, sizeof(msg), 3, &total);
    TEST_ASSERT_GREATER_THAN(0, (int)total);

    pg_parsed_msg out;
    int rc = pg_parse_message(msg, total, &out);
    TEST_ASSERT_EQUAL_INT((int)total, rc);
    TEST_ASSERT_EQUAL_INT(PG_MSG_ROW_DESCRIPTION, (int)out.type);
    TEST_ASSERT_EQUAL_INT16(3, out.row_description.ncols);
}

void test_parse_incomplete_returns_zero(void) {
    /* Truncate the auth_ok message at every byte position */
    const char full[] = "\x52\x00\x00\x00\x08\x00\x00\x00\x00";
    pg_parsed_msg out;
    for (size_t i = 0; i < 9; i++) {
        int rc = pg_parse_message(full, i, &out);
        TEST_ASSERT_EQUAL_INT(0, rc);
    }
}

void test_parse_unknown_tag_consumed(void) {
    /* Tag 'X' (unknown) with valid length — should be consumed as UNKNOWN */
    char msg[8];
    msg[0] = 'X';
    msg[1] = 0; msg[2] = 0; msg[3] = 0; msg[4] = 6;  /* msglen=6, body=2 */
    msg[5] = 0xab; msg[6] = 0xcd;
    pg_parsed_msg out;
    int rc = pg_parse_message(msg, 7, &out);
    TEST_ASSERT_EQUAL_INT(7, rc);
    TEST_ASSERT_EQUAL_INT(PG_MSG_UNKNOWN, (int)out.type);
}

/* =========================================================================
 * P6-2 encoder tests
 * ====================================================================== */

void test_encode_parse_anonymous(void) {
    char buf[128];
    const char *sql = "SELECT $1::int";
    int n = pg_encode_parse(buf, sizeof(buf), "", sql);
    /* 'P' + int32(len) + "\0" + sql + "\0" + int16(0) */
    size_t sql_len = strlen(sql);
    int expected = (int)(1 + 4 + 1 + sql_len + 1 + 2);
    TEST_ASSERT_EQUAL_INT(expected, n);
    TEST_ASSERT_EQUAL_CHAR('P', buf[0]);
    TEST_ASSERT_EQUAL_CHAR('\0', buf[5]);   /* empty name */
    TEST_ASSERT_EQUAL_MEMORY(sql, buf + 6, sql_len);
    TEST_ASSERT_EQUAL_CHAR('\0', buf[6 + sql_len]);
    /* int16(0) at end */
    TEST_ASSERT_EQUAL_INT16(0, rd_i16(buf + 7 + sql_len));
}

void test_encode_parse_named(void) {
    char buf[128];
    int n = pg_encode_parse(buf, sizeof(buf), "mystmt", "SELECT 1");
    TEST_ASSERT_GREATER_THAN_INT(0, n);
    TEST_ASSERT_EQUAL_CHAR('P', buf[0]);
    TEST_ASSERT_EQUAL_MEMORY("mystmt\0", buf + 5, 7);
    TEST_ASSERT_EQUAL_MEMORY("SELECT 1\0", buf + 12, 9);
}

void test_encode_bind_no_params(void) {
    char buf[64];
    int n = pg_encode_bind(buf, sizeof(buf), "", "", NULL, NULL, 0);
    TEST_ASSERT_GREATER_THAN_INT(0, n);
    TEST_ASSERT_EQUAL_CHAR('B', buf[0]);
    /* portal="\0" at buf+5, stmt="\0" at buf+6 */
    TEST_ASSERT_EQUAL_CHAR('\0', buf[5]);
    TEST_ASSERT_EQUAL_CHAR('\0', buf[6]);
    /* int16(0) format codes */
    TEST_ASSERT_EQUAL_INT16(0, rd_i16(buf + 7));
    /* int16(0) params */
    TEST_ASSERT_EQUAL_INT16(0, rd_i16(buf + 9));
    /* int16(0) result format codes */
    TEST_ASSERT_EQUAL_INT16(0, rd_i16(buf + 11));
}

void test_encode_bind_two_text_params(void) {
    char buf[128];
    const char *params[2]   = {"hello", "42"};
    size_t      lens[2]     = {5, 2};
    int n = pg_encode_bind(buf, sizeof(buf), "", "", params, lens, 2);
    TEST_ASSERT_GREATER_THAN_INT(0, n);
    TEST_ASSERT_EQUAL_CHAR('B', buf[0]);
    /* n_params = 2 at offset: 1+4 + 1 + 1 + 2 = 9 */
    int16_t n_params = rd_i16(buf + 9);
    TEST_ASSERT_EQUAL_INT16(2, n_params);
    /* first param length = 5 */
    TEST_ASSERT_EQUAL_INT32(5, rd_i32(buf + 11));
    TEST_ASSERT_EQUAL_MEMORY("hello", buf + 15, 5);
    /* second param length = 2 */
    TEST_ASSERT_EQUAL_INT32(2, rd_i32(buf + 20));
    TEST_ASSERT_EQUAL_MEMORY("42", buf + 24, 2);
}

void test_encode_bind_null_param(void) {
    char buf[64];
    const char *params[2]  = {NULL, "x"};
    size_t      lens[2]    = {0, 1};
    int n = pg_encode_bind(buf, sizeof(buf), "", "", params, lens, 2);
    TEST_ASSERT_GREATER_THAN_INT(0, n);
    /* first param: int32(-1) = null */
    int32_t first_len = rd_i32(buf + 11);
    TEST_ASSERT_EQUAL_INT32(-1, first_len);
    /* second param: int32(1) + 'x' */
    TEST_ASSERT_EQUAL_INT32(1, rd_i32(buf + 15));
    TEST_ASSERT_EQUAL_CHAR('x', buf[19]);
}

void test_encode_execute_anonymous(void) {
    char buf[32];
    int n = pg_encode_execute(buf, sizeof(buf), "", 0);
    /* 'E' + int32(len) + "\0" + int32(0) */
    int expected = 1 + 4 + 1 + 4;
    TEST_ASSERT_EQUAL_INT(expected, n);
    TEST_ASSERT_EQUAL_CHAR('E', buf[0]);
    TEST_ASSERT_EQUAL_CHAR('\0', buf[5]);
    TEST_ASSERT_EQUAL_INT32(0, rd_i32(buf + 6));
}

void test_encode_execute_row_limit(void) {
    char buf[32];
    int n = pg_encode_execute(buf, sizeof(buf), "", 10);
    TEST_ASSERT_GREATER_THAN_INT(0, n);
    TEST_ASSERT_EQUAL_INT32(10, rd_i32(buf + 6));
}

void test_encode_sync(void) {
    char buf[16];
    int n = pg_encode_sync(buf, sizeof(buf));
    TEST_ASSERT_EQUAL_INT(5, n);
    TEST_ASSERT_EQUAL_CHAR('S', buf[0]);
    TEST_ASSERT_EQUAL_INT32(4, rd_i32(buf + 1));
}

void test_encode_describe_statement(void) {
    char buf[32];
    int n = pg_encode_describe(buf, sizeof(buf), 'S', "mystmt");
    /* 'D' + int32(len) + 'S' + "mystmt\0" */
    int expected = 1 + 4 + 1 + 6 + 1;
    TEST_ASSERT_EQUAL_INT(expected, n);
    TEST_ASSERT_EQUAL_CHAR('D', buf[0]);
    TEST_ASSERT_EQUAL_CHAR('S', buf[5]);
    TEST_ASSERT_EQUAL_MEMORY("mystmt\0", buf + 6, 7);
}

void test_encode_describe_portal(void) {
    char buf[16];
    int n = pg_encode_describe(buf, sizeof(buf), 'P', "");
    int expected = 1 + 4 + 1 + 1;
    TEST_ASSERT_EQUAL_INT(expected, n);
    TEST_ASSERT_EQUAL_CHAR('D', buf[0]);
    TEST_ASSERT_EQUAL_CHAR('P', buf[5]);
    TEST_ASSERT_EQUAL_CHAR('\0', buf[6]);
}

void test_encode_close_stmt(void) {
    char buf[32];
    int n = pg_encode_close_stmt(buf, sizeof(buf), "mystmt");
    int expected = 1 + 4 + 1 + 6 + 1;
    TEST_ASSERT_EQUAL_INT(expected, n);
    TEST_ASSERT_EQUAL_CHAR('C', buf[0]);
    TEST_ASSERT_EQUAL_CHAR('S', buf[5]);
    TEST_ASSERT_EQUAL_MEMORY("mystmt\0", buf + 6, 7);
}

/* =========================================================================
 * P6-2 parser tests
 * ====================================================================== */

/* Build a RowDescription with named columns (name + \0 + 18 fixed) */
static void build_named_row_desc(char *buf, size_t cap,
                                 const char **names, int ncols,
                                 size_t *out_total) {
    /* compute body size */
    size_t body_len = 2;
    for (int i = 0; i < ncols; i++)
        body_len += strlen(names[i]) + 1 + 18;
    int32_t msglen = (int32_t)(4 + body_len);
    *out_total = 1 + (size_t)msglen;
    if (*out_total > cap) { *out_total = 0; return; }
    buf[0] = 'T';
    buf[1] = (char)((msglen >> 24) & 0xff);
    buf[2] = (char)((msglen >> 16) & 0xff);
    buf[3] = (char)((msglen >>  8) & 0xff);
    buf[4] = (char)( msglen        & 0xff);
    buf[5] = (char)((ncols >> 8) & 0xff);
    buf[6] = (char)( ncols       & 0xff);
    char *p = buf + 7;
    for (int i = 0; i < ncols; i++) {
        size_t nlen = strlen(names[i]);
        memcpy(p, names[i], nlen); p += nlen; *p++ = '\0';
        memset(p, 0, 18); p += 18;
    }
}

void test_parse_row_description_column_names(void) {
    const char *names[] = {"id", "val"};
    char msg[256];
    size_t total = 0;
    build_named_row_desc(msg, sizeof(msg), names, 2, &total);
    TEST_ASSERT_GREATER_THAN(0, (int)total);

    pg_parsed_msg out;
    int rc = pg_parse_message(msg, total, &out);
    TEST_ASSERT_EQUAL_INT((int)total, rc);
    TEST_ASSERT_EQUAL_INT(PG_MSG_ROW_DESCRIPTION, (int)out.type);
    TEST_ASSERT_EQUAL_INT16(2, out.row_description.ncols);
    TEST_ASSERT_EQUAL_STRING("id",  out.row_description.cols[0].name);
    TEST_ASSERT_EQUAL_STRING("val", out.row_description.cols[1].name);
}

void test_parse_row_description_name_truncation(void) {
    /* column name = 80 'a' chars → truncated to 63 in cols[0].name */
    char long_name[81];
    memset(long_name, 'a', 80);
    long_name[80] = '\0';
    const char *names[] = {long_name};
    char msg[512];
    size_t total = 0;
    build_named_row_desc(msg, sizeof(msg), names, 1, &total);
    TEST_ASSERT_GREATER_THAN(0, (int)total);

    pg_parsed_msg out;
    int rc = pg_parse_message(msg, total, &out);
    TEST_ASSERT_EQUAL_INT((int)total, rc);
    TEST_ASSERT_EQUAL_INT(PG_MSG_ROW_DESCRIPTION, (int)out.type);
    /* name must fit in 64-byte field: 63 chars + '\0' */
    TEST_ASSERT_EQUAL_CHAR('\0', out.row_description.cols[0].name[63]);
    /* first 63 chars must all be 'a' */
    char expected63[64];
    memset(expected63, 'a', 63);
    expected63[63] = '\0';
    TEST_ASSERT_EQUAL_MEMORY(expected63, out.row_description.cols[0].name, 64);
}

void test_parse_row_description_clamp_at_max_cols(void) {
    /* Build a RowDescription with PG_MAX_COLS + 1 = 65 columns */
    int ncols = PG_MAX_COLS + 1;
    /* per-column: "cx\0" + 18 = 21 bytes */
    size_t body_len = 2 + (size_t)ncols * 21;
    int32_t msglen  = (int32_t)(4 + body_len);
    size_t total    = 1 + (size_t)msglen;

    char *msg = malloc(total);
    TEST_ASSERT_NOT_NULL(msg);
    msg[0] = 'T';
    msg[1] = (char)((msglen >> 24) & 0xff);
    msg[2] = (char)((msglen >> 16) & 0xff);
    msg[3] = (char)((msglen >>  8) & 0xff);
    msg[4] = (char)( msglen        & 0xff);
    msg[5] = (char)((ncols >> 8) & 0xff);
    msg[6] = (char)( ncols       & 0xff);
    char *p = msg + 7;
    for (int i = 0; i < ncols; i++) {
        *p++ = 'c'; *p++ = 'x'; *p++ = '\0';
        memset(p, 0, 18); p += 18;
    }

    pg_parsed_msg out;
    int rc = pg_parse_message(msg, total, &out);
    free(msg);

    TEST_ASSERT_EQUAL_INT((int)total, rc);
    TEST_ASSERT_EQUAL_INT(PG_MSG_ROW_DESCRIPTION, (int)out.type);
    /* clamped to PG_MAX_COLS (64) */
    TEST_ASSERT_EQUAL_INT16((int16_t)PG_MAX_COLS, out.row_description.ncols);
    /* last accessible column name is valid */
    TEST_ASSERT_EQUAL_STRING("cx", out.row_description.cols[PG_MAX_COLS - 1].name);
}

void test_parse_parse_complete(void) {
    const char msg[] = "\x31\x00\x00\x00\x04";
    pg_parsed_msg out;
    int rc = pg_parse_message(msg, 5, &out);
    TEST_ASSERT_EQUAL_INT(5, rc);
    TEST_ASSERT_EQUAL_INT(PG_MSG_PARSE_COMPLETE, (int)out.type);
}

void test_parse_bind_complete(void) {
    const char msg[] = "\x32\x00\x00\x00\x04";
    pg_parsed_msg out;
    int rc = pg_parse_message(msg, 5, &out);
    TEST_ASSERT_EQUAL_INT(5, rc);
    TEST_ASSERT_EQUAL_INT(PG_MSG_BIND_COMPLETE, (int)out.type);
}

void test_parse_close_complete(void) {
    const char msg[] = "\x33\x00\x00\x00\x04";
    pg_parsed_msg out;
    int rc = pg_parse_message(msg, 5, &out);
    TEST_ASSERT_EQUAL_INT(5, rc);
    TEST_ASSERT_EQUAL_INT(PG_MSG_CLOSE_COMPLETE, (int)out.type);
}

void test_parse_no_data(void) {
    const char msg[] = "\x6e\x00\x00\x00\x04";
    pg_parsed_msg out;
    int rc = pg_parse_message(msg, 5, &out);
    TEST_ASSERT_EQUAL_INT(5, rc);
    TEST_ASSERT_EQUAL_INT(PG_MSG_NO_DATA, (int)out.type);
}

void test_parse_parameter_description_zero(void) {
    /* 't' + int32(6) + int16(0) = 7 bytes */
    const char msg[] = "\x74\x00\x00\x00\x06\x00\x00";
    pg_parsed_msg out;
    int rc = pg_parse_message(msg, 7, &out);
    TEST_ASSERT_EQUAL_INT(7, rc);
    TEST_ASSERT_EQUAL_INT(PG_MSG_PARAMETER_DESCRIPTION, (int)out.type);
    TEST_ASSERT_EQUAL_INT16(0, out.param_description.n_params);
}

void test_encode_parse_sync_sequence_lengths(void) {
    /* Encode Parse("", "SELECT 1") then Sync; walk the buffer by tag + int32
     * length to verify layout.  pg_parse_message is NOT called here — it is a
     * backend-response parser and must not be fed frontend frames. */
    char buf[256];
    int parse_n = pg_encode_parse(buf, sizeof(buf), "", "SELECT 1");
    int sync_n  = pg_encode_sync(buf + parse_n, sizeof(buf) - (size_t)parse_n);
    TEST_ASSERT_GREATER_THAN_INT(0, parse_n);
    TEST_ASSERT_EQUAL_INT(5, sync_n);

    /* Walk by tag + length */
    TEST_ASSERT_EQUAL_CHAR('P', buf[0]);
    int32_t parse_len = rd_i32(buf + 1);
    int parse_total = 1 + (int)parse_len;
    TEST_ASSERT_EQUAL_INT(parse_n, parse_total);

    int sync_offset = parse_n;
    TEST_ASSERT_EQUAL_CHAR('S', buf[sync_offset]);
    int32_t sync_len = rd_i32(buf + sync_offset + 1);
    TEST_ASSERT_EQUAL_INT32(4, sync_len);

    int total_bytes = parse_n + sync_n;
    TEST_ASSERT_EQUAL_INT(total_bytes, parse_total + 5);
}

/* =========================================================================
 * main
 * ====================================================================== */

int main(void) {
    UNITY_BEGIN();

    /* P6-1 encoders */
    RUN_TEST(test_encode_startup_format);
    RUN_TEST(test_encode_query_format);
    RUN_TEST(test_encode_password_cleartext);
    RUN_TEST(test_encode_md5_password);

    /* P6-1 parser */
    RUN_TEST(test_parse_auth_ok);
    RUN_TEST(test_parse_auth_cleartext);
    RUN_TEST(test_parse_auth_md5);
    RUN_TEST(test_parse_auth_sasl);
    RUN_TEST(test_parse_ready_for_query);
    RUN_TEST(test_parse_command_complete);
    RUN_TEST(test_parse_error_response);
    RUN_TEST(test_parse_data_row);
    RUN_TEST(test_parse_row_description);
    RUN_TEST(test_parse_incomplete_returns_zero);
    RUN_TEST(test_parse_unknown_tag_consumed);

    /* P6-2 encoders */
    RUN_TEST(test_encode_parse_anonymous);
    RUN_TEST(test_encode_parse_named);
    RUN_TEST(test_encode_bind_no_params);
    RUN_TEST(test_encode_bind_two_text_params);
    RUN_TEST(test_encode_bind_null_param);
    RUN_TEST(test_encode_execute_anonymous);
    RUN_TEST(test_encode_execute_row_limit);
    RUN_TEST(test_encode_sync);
    RUN_TEST(test_encode_describe_statement);
    RUN_TEST(test_encode_describe_portal);
    RUN_TEST(test_encode_close_stmt);

    /* P6-2 parser */
    RUN_TEST(test_parse_row_description_column_names);
    RUN_TEST(test_parse_row_description_name_truncation);
    RUN_TEST(test_parse_row_description_clamp_at_max_cols);
    RUN_TEST(test_parse_parse_complete);
    RUN_TEST(test_parse_bind_complete);
    RUN_TEST(test_parse_close_complete);
    RUN_TEST(test_parse_no_data);
    RUN_TEST(test_parse_parameter_description_zero);
    RUN_TEST(test_encode_parse_sync_sequence_lengths);

    return UNITY_END();
}
