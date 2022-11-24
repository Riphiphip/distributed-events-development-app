#include <zephyr/fff.h>
#include <zephyr/random/rand32.h>
#include <zephyr/ztest.h>

#include "../../drivers/zephyr,uart-ipc-service-backend.c"

DEFINE_FFF_GLOBALS;

/* Fakes */
FAKE_VOID_FUNC(fake_endpoint_cb_received, const void *, size_t, void *);
FAKE_VOID_FUNC(fake_endpoint_cb_bound, void *);
FAKE_VOID_FUNC(fake_endpoint_cb_error, const char *, void *);

void endpoint_error_callback_default(const char *message, void *priv) {
    fake_endpoint_cb_error(message, priv);  // Call to track number of error callbacks
    LOG_ERR("Endpoint error callback: %s", message);
}

#define FAKE_LIST(OP)             \
    OP(fake_endpoint_cb_received) \
    OP(fake_endpoint_cb_bound)    \
    OP(fake_endpoint_cb_error)

static struct uart_ipc_service_backend_suite_fixture {
    struct backend_data instance_data;
    struct device instance;
    struct uart_event uart_event;
    struct uart_ipc_frame frame;
    struct buffer_container {
        void **buffers;
        size_t count;
        size_t max;
    } buffer_container;  // Used for tracking heap allocated buffers that need to be freed after each test.
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
        .error = endpoint_error_callback_default,
    };

    fixture->buffer_container.max = 2;
    fixture->buffer_container.buffers = k_malloc(sizeof(void *) * fixture->buffer_container.max);

    k_work_init(&fixture->instance_data.free_tx_work, free_tx_work_handler);
    k_work_init_delayable(&fixture->instance_data.endpoint.rx_timeout_work, endpoint_rx_timeout_handler);
}

static void suite_after(void *f) {
    struct uart_ipc_service_backend_suite_fixture *fixture = f;

    for (size_t i = 0; i < fixture->buffer_container.count; i++) {
        k_free(fixture->buffer_container.buffers[i]);
    }

    k_free(fixture->buffer_container.buffers);
}

static void suite_teardown(void *f) {
    k_free(f);
}

ZTEST_SUITE(uart_ipc_service_backend_suite, NULL, suite_setup, suite_before, suite_after, suite_teardown);

/* Utility */

static struct sized_buffer {
    size_t size;
    void *data;
};

static int register_test_buffer(void *buffer, struct uart_ipc_service_backend_suite_fixture *fixture) {

    fixture->buffer_container.buffers[fixture->buffer_container.count] = buffer;
    fixture->buffer_container.count++;

    if (fixture->buffer_container.count >= fixture->buffer_container.max) {
        void *new_buf = k_malloc(sizeof(void *) * fixture->buffer_container.max * 2);
        memcpy(new_buf, fixture->buffer_container.buffers, sizeof(void *) * fixture->buffer_container.max);
        k_free(fixture->buffer_container.buffers);
        fixture->buffer_container.buffers = new_buf;
        fixture->buffer_container.max *= 2;
    }
}

static int free_test_buffer(void *buffer, struct uart_ipc_service_backend_suite_fixture *fixture) {
    k_free(buffer);
    for (size_t i = 0; i < fixture->buffer_container.count; i++) {
        if (fixture->buffer_container.buffers[i] == buffer) {
            fixture->buffer_container.buffers[i] = NULL;
            return 0;
        }
    }
    return 0;
}

/* Endpoint callback functions */

/* Error callbacks */
void endpoint_error_callback_expect_frame_wrong_size(const char *message, void *priv) {
    fake_endpoint_cb_error(message, priv);  // Call to track number of error callbacks
    char msg[] = "Received data is not a valid frame. IPC instance is in an invalid state";
    zassert_mem_equal(message, msg, sizeof(msg), "Wrong error message");
}

/* Receive callbacks */

void endpoint_receive_callback_validate_data(const void *data, size_t len, void *priv) {
    fake_endpoint_cb_received(data, len, priv);  // Call to track number of receive callbacks

    struct sized_buffer *expected_result = (struct sized_buffer *)priv;

    zassert_equal(len, expected_result->size, "Wrong data size");
    zassert_mem_equal(data, expected_result->data, len, "Wrong data");
}

/*================================= Tests ===============================*/

ZTEST_F(uart_ipc_service_backend_suite, test_error_frame_wrong_size) {
    fixture->uart_event = (struct uart_event){
        .type = UART_RX_RDY,
        .data.rx.buf = NULL,
        .data.rx.len = sizeof(struct uart_ipc_frame) - 1,  // Received 1 byte less than expected
    };

    fixture->instance_data.endpoint.cfg.cb.error = endpoint_error_callback_expect_frame_wrong_size;

    uart_callback(NULL, &fixture->uart_event, &fixture->instance);

    zassert_equal(1, fake_endpoint_cb_error_fake.call_count, "Wrong number of calls to endpoint error callback");
    zassert_equal(0, fake_endpoint_cb_received_fake.call_count, "Wrong number of calls to endpoint received callback");
    zassert_equal(0, fake_endpoint_cb_bound_fake.call_count, "Wrong number of calls to endpoint bound callback");
}

ZTEST_F(uart_ipc_service_backend_suite, test_roundtrip_data_frame_creation) {
    size_t data_length = 3 * sizeof(((struct uart_ipc_frame *)0)->frag) + 7;
    const uint8_t data[data_length];
    zassert_not_null(data, "Failed to allocate memory for test data");

    sys_rand_get(data, data_length);
    size_t n_frames = 0;
    struct uart_ipc_frame *frames = create_frames(data, data_length, &n_frames);

    register_test_buffer(frames, fixture);

    zassert_equal(n_frames > 0, true, "Frame count should be greater than 0");

    const uint8_t unwrapped_data[data_length];
    zassert_not_null(unwrapped_data, "Failed to allocate memory for unwrapped data");

    size_t total_unwrapped_bytes = 0;

    for (int i = 0; i < n_frames; ++i) {
        struct uart_ipc_frame *frame = &frames[i];
        size_t unwrapped_bytes = 0;
        int err = unwrap_frame(unwrapped_data, data_length, frame, &unwrapped_bytes);
        zassert_equal(err, 0, "Failed to unwrap frame %d/%d. Error code: %d", i, n_frames, err);
        total_unwrapped_bytes += unwrapped_bytes;
        zassert_equal(total_unwrapped_bytes > data_length, false,
                      "Unwrapped data is larger than original data after %d/%d frames", i, n_frames);
    }
    free_test_buffer(frames, fixture);

    zassert_equal(total_unwrapped_bytes, data_length, "Unwrapped data is not the same size as original data. Expected %d, got %d",
                  data_length, total_unwrapped_bytes);

    zassert_mem_equal(data, unwrapped_data, data_length, "Unwrapped data is not the same as original data");
}

ZTEST_F(uart_ipc_service_backend_suite, test_one_frame_received_successfully) {
    uint16_t total_data_length = sizeof(((struct uart_ipc_frame *)0)->frag);
    uint8_t *data = k_malloc(total_data_length);
    register_test_buffer(data, fixture);

    sys_rand_get(data, total_data_length);

    size_t n_frames = 0;
    struct uart_ipc_frame *frames = create_frames(data, total_data_length, &n_frames);
    register_test_buffer(frames, fixture);

    zassert_equal(n_frames, 1, "Wrong number of frames");

    fixture->uart_event = (struct uart_event){
        .type = UART_RX_RDY,
        .data.rx.buf = frames,
        .data.rx.len = sizeof(struct uart_ipc_frame) * n_frames,
    };

    fixture->instance_data.endpoint.cfg.cb.received = endpoint_receive_callback_validate_data;

    struct sized_buffer expected_result = {
        .size = total_data_length,
        .data = data,
    };

    fixture->instance_data.endpoint.cfg.priv = &expected_result;

    uart_callback(NULL, &fixture->uart_event, &fixture->instance);

    zassert_equal(fake_endpoint_cb_error_fake.call_count, 0, "Wrong number of calls to endpoint error callback");
    zassert_equal(fake_endpoint_cb_received_fake.call_count, 1, "Wrong number of calls to endpoint received callback");
    zassert_equal(fake_endpoint_cb_bound_fake.call_count, 0, "Wrong number of calls to endpoint bound callback");
}

ZTEST_F(uart_ipc_service_backend_suite, test_multiple_frames_received_successfully) {

    fixture->instance_data.endpoint.cfg.cb.received = endpoint_receive_callback_validate_data;

    uint16_t total_data_length = sizeof(((struct uart_ipc_frame *)0)->frag) * 10 + 5;
    uint8_t *data = k_malloc(total_data_length);
    register_test_buffer(data, fixture);

    sys_rand_get(data, total_data_length);

    size_t n_frames = 0;
    struct uart_ipc_frame *frames = create_frames(data, total_data_length, &n_frames);
    register_test_buffer(frames, fixture);

    zassert_equal(n_frames > 0, true, "Wrong number of frames");

    struct sized_buffer expected_result = {
        .size = total_data_length,
        .data = data,
    };
    fixture->instance_data.endpoint.cfg.priv = &expected_result;

    for (int i = 0; i < n_frames; ++i){
        fixture->uart_event = (struct uart_event){
            .type = UART_RX_RDY,
            .data.rx.buf = &frames[i],
            .data.rx.len = sizeof(struct uart_ipc_frame),
        };
        uart_callback(NULL, &fixture->uart_event, &fixture->instance);
    } 

    zassert_equal(fake_endpoint_cb_error_fake.call_count, 0, "Called %d times", fake_endpoint_cb_error_fake.call_count);
    zassert_equal(fake_endpoint_cb_received_fake.call_count, 1, "Called %d times", fake_endpoint_cb_received_fake.call_count);
    zassert_equal(fake_endpoint_cb_bound_fake.call_count, 0, "Called %d times", fake_endpoint_cb_bound_fake.call_count);
}
