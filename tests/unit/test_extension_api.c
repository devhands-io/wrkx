/* tests/unit/test_extension_api.c
 *
 * Unit tests for the wrkx extension API/ABI boundary (ADR 0005, P3-1).
 *
 * Tests verify:
 *   - The wrkx_extension_api version field matches WRKX_EXTENSION_API_VERSION
 *   - The toy extension's init function registers a protocol and helpers
 *   - An extension that detects a version mismatch registers nothing
 *   - An extension that receives a NULL api pointer does not crash
 *
 * No scripting engine, no LuaJIT, no orchestrator — only wrkx_extension.h
 * and the toy extension source are linked.
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "unity.h"
#include "wrkx_extension.h"

/* Declaration of the toy extension entry point (compiled from extensions/toy/toy.c). */
void wrkx_extension_init_toy(const wrkx_extension_api *api);

/* -------------------------------------------------------------------------
 * Mock API callbacks
 * ---------------------------------------------------------------------- */

static int            g_protocol_registered;
static int            g_helpers_registered;
static const protocol *g_last_proto;
static const char     *g_last_ns;
static size_t          g_last_count;

static void mock_register_protocol(const protocol *p) {
    g_protocol_registered++;
    g_last_proto = p;
}

static void mock_register_helpers(const char *ns, const script_helper *h,
                                  size_t count) {
    (void)h;
    g_helpers_registered++;
    g_last_ns    = ns;
    g_last_count = count;
}

static wrkx_extension_api make_api(void) {
    wrkx_extension_api a;
    memset(&a, 0, sizeof(a));
    a.version            = WRKX_EXTENSION_API_VERSION;
    a.register_protocol  = mock_register_protocol;
    a.register_helpers   = mock_register_helpers;
    return a;
}

/* -------------------------------------------------------------------------
 * Fixture
 * ---------------------------------------------------------------------- */

void setUp(void) {
    g_protocol_registered = 0;
    g_helpers_registered  = 0;
    g_last_proto  = NULL;
    g_last_ns     = NULL;
    g_last_count  = 0;
}

void tearDown(void) {}

/* -------------------------------------------------------------------------
 * Tests
 * ---------------------------------------------------------------------- */

void test_api_version_matches(void) {
    wrkx_extension_api api = make_api();
    TEST_ASSERT_EQUAL_UINT32(WRKX_EXTENSION_API_VERSION, api.version);
}

void test_extension_init_registers_something(void) {
    wrkx_extension_api api = make_api();
    wrkx_extension_init_toy(&api);
    TEST_ASSERT_TRUE(g_protocol_registered > 0 || g_helpers_registered > 0);
}

void test_register_protocol_invoked(void) {
    wrkx_extension_api api = make_api();
    wrkx_extension_init_toy(&api);
    TEST_ASSERT_EQUAL_INT(1, g_protocol_registered);
    TEST_ASSERT_NOT_NULL(g_last_proto);
    TEST_ASSERT_NOT_NULL(g_last_proto->name);
    TEST_ASSERT_EQUAL_STRING("toy", g_last_proto->name);
}

void test_register_helpers_invoked(void) {
    wrkx_extension_api api = make_api();
    wrkx_extension_init_toy(&api);
    TEST_ASSERT_EQUAL_INT(1, g_helpers_registered);
    TEST_ASSERT_NOT_NULL(g_last_ns);
    TEST_ASSERT_EQUAL_STRING("toy", g_last_ns);
    TEST_ASSERT_GREATER_THAN(0, (int)g_last_count);
}

void test_wrong_version_registers_nothing(void) {
    wrkx_extension_api api = make_api();
    api.version = WRKX_EXTENSION_API_VERSION + 1;
    wrkx_extension_init_toy(&api);
    TEST_ASSERT_EQUAL_INT(0, g_protocol_registered);
    TEST_ASSERT_EQUAL_INT(0, g_helpers_registered);
}

void test_null_api_does_not_crash(void) {
    /* Extension must guard against NULL before dereferencing api. */
    wrkx_extension_init_toy(NULL);
    TEST_ASSERT_EQUAL_INT(0, g_protocol_registered);
    TEST_ASSERT_EQUAL_INT(0, g_helpers_registered);
}

/* -------------------------------------------------------------------------
 * main
 * ---------------------------------------------------------------------- */

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_api_version_matches);
    RUN_TEST(test_extension_init_registers_something);
    RUN_TEST(test_register_protocol_invoked);
    RUN_TEST(test_register_helpers_invoked);
    RUN_TEST(test_wrong_version_registers_nothing);
    RUN_TEST(test_null_api_does_not_crash);
    return UNITY_END();
}
