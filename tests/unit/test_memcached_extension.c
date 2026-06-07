/*
 * Unit tests for the memcached extension skeleton (ADR 0005, P4-1, t058).
 *
 * No network, no scripting engine, no orchestrator. This only proves the
 * extension entry point fits the public API and registers the stub protocol.
 */

#include <string.h>

#include "unity.h"
#include "wrkx_extension.h"

void wrkx_extension_init_memcached(const wrkx_extension_api *api);

static const protocol *registered_protocol;
static const char     *registered_ns;
static size_t          registered_helper_count;

static void mock_register_protocol(const protocol *p) {
    registered_protocol = p;
}

static void mock_register_helpers(const char *ns, const script_helper *h,
                                  size_t count) {
    (void)h;
    registered_ns = ns;
    registered_helper_count = count;
}

static void mock_register_schema(const char *schema, const char *schema_tls,
                                 const char *default_port,
                                 wrkx_configure_fn configure) {
    (void)schema;
    (void)schema_tls;
    (void)default_port;
    (void)configure;
}

static wrkx_extension_api make_api(void) {
    wrkx_extension_api api;
    api.version           = WRKX_EXTENSION_API_VERSION;
    api.register_protocol = mock_register_protocol;
    api.register_helpers  = mock_register_helpers;
    api.register_schema   = mock_register_schema;
    return api;
}

void setUp(void) {
    registered_protocol = NULL;
    registered_ns = NULL;
    registered_helper_count = 0;
}

void tearDown(void) {}

void test_memcached_init_registers_protocol(void) {
    wrkx_extension_api api = make_api();

    wrkx_extension_init_memcached(&api);

    TEST_ASSERT_NOT_NULL(registered_protocol);
    TEST_ASSERT_EQUAL_STRING("memcached", registered_protocol->name);
    TEST_ASSERT_NOT_NULL(registered_protocol->connect);
    TEST_ASSERT_NOT_NULL(registered_protocol->write);
    TEST_ASSERT_NOT_NULL(registered_protocol->readable);
    TEST_ASSERT_NOT_NULL(registered_protocol->close);
}

void test_memcached_init_registers_helpers(void) {
    wrkx_extension_api api = make_api();

    wrkx_extension_init_memcached(&api);

    TEST_ASSERT_NOT_NULL(registered_ns);
    TEST_ASSERT_EQUAL_STRING("memcached", registered_ns);
    TEST_ASSERT_EQUAL_UINT(5, registered_helper_count); /* get set delete incr decr */
}

void test_memcached_init_rejects_wrong_version(void) {
    wrkx_extension_api api = make_api();
    api.version++;

    wrkx_extension_init_memcached(&api);

    TEST_ASSERT_NULL(registered_protocol);
}

void test_memcached_init_accepts_null_api(void) {
    wrkx_extension_init_memcached(NULL);
    TEST_ASSERT_NULL(registered_protocol);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_memcached_init_registers_protocol);
    RUN_TEST(test_memcached_init_registers_helpers);
    RUN_TEST(test_memcached_init_rejects_wrong_version);
    RUN_TEST(test_memcached_init_accepts_null_api);
    return UNITY_END();
}

