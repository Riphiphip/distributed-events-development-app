#include <zephyr/fff.h>
#include <zephyr/ztest.h>

#include "../../drivers/zephyr,uart-ipc-service-backend.c"

DEFINE_FFF_GLOBALS;

/* Fakes */
FAKE_VOID_FUNC(fake_endpoint_cb_received, const void *, size_t, void *);
FAKE_VOID_FUNC(fake_endpoint_cb_bound, void *);
FAKE_VOID_FUNC(fake_endpoint_cb_error, const char *, void *);

#define FAKE_LIST(OP)             \
    OP(fake_endpoint_cb_received) \
    OP(fake_endpoint_cb_bound)    \
    OP(fake_endpoint_cb_error)

static struct uart_ipc_service_backend_suite_fixture {
    struct backend_data instance_data;
    struct device instance;
    struct uart_event uart_event;
};

static void *suite_setup(void) {
    struct uart_ipc_service_backend_suite_fixture *fixture = k_malloc(sizeof(*fixture));
    zassume_not_null(fixture, "Failed to allocate memory for test fixture. Skipping test suite");

    return fixture;
}

static void suite_before(void *f) {
    struct uart_ipc_service_backend_suite_fixture *fixture = f;

    FAKE_LIST(RESET_FAKE);
    FFF_RESET_HISTORY();

    memset(fixture, 0, sizeof(*fixture));

    fixture->instance.data = &fixture->instance_data;
    fixture->instance_data.endpoint.cfg.cb = (struct ipc_service_cb){
        .received = fake_endpoint_cb_received,
        .bound = fake_endpoint_cb_bound,
        .error = fake_endpoint_cb_error,
    };
}

static void suite_teardown(void *f) {
    k_free(f);
}

ZTEST_SUITE(uart_ipc_service_backend_suite, NULL, suite_setup, suite_before, NULL, suite_teardown);

/*================================= Tests ===============================*/

void endpoint_error_callback_expect_frame_wrong_size(const char *message, void *priv) {
    fake_endpoint_cb_error(message, priv); // Call to track number of error callbacks
    char msg[] = "Received data is not a valid frame. IPC instance is in an invalid state";
    zassert_mem_equal(message, msg, sizeof(msg), "Wrong error message");
}

ZTEST_F(uart_ipc_service_backend_suite, test_error_frame_wrong_size) {

    fixture->uart_event = (struct uart_event){
        .type = UART_RX_RDY,
        .data.rx.buf = NULL,
        .data.rx.len = sizeof(struct uart_ipc_frame) -1, // Received 1 byte less than expected
    };

    fixture->instance_data.endpoint.cfg.cb.error = endpoint_error_callback_expect_frame_wrong_size;


    uart_callback(NULL, &fixture->uart_event, &fixture->instance);

    zassert_equal(1, fake_endpoint_cb_error_fake.call_count, "Wrong number of calls to endpoint error callback");
    zassert_equal(0, fake_endpoint_cb_received_fake.call_count, "Wrong number of calls to endpoint received callback");
    zassert_equal(0, fake_endpoint_cb_bound_fake.call_count, "Wrong number of calls to endpoint bound callback");
}
