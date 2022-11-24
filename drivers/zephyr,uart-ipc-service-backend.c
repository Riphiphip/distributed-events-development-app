#include <devicetree.h>
#include <logging/log.h>
#include <string.h>
#include <sys/util.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/ipc/ipc_service_backend.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/crc.h>
LOG_MODULE_REGISTER(IPC_BACKEND_UART, CONFIG_IPC_BACKEND_UART_LOG_LEVEL);

#define DT_DRV_COMPAT zephyr_uart_ipc_service_backend

static inline void uart_callback(const struct device *dev, struct uart_event *evt, void *user_data);  // Forward declaration for readability

struct uart_ipc_frame {
    uint16_t total_data_length;  // Total length of the data in the transfer
    uint16_t frag_start;         // Offset of the fragment in the transfer
    uint8_t frag_len;            // Length of the fragment
    uint8_t frag[64];            // Data fragment
    uint32_t crc;                // crc32-ieee for the frame
} __packed__;

enum ept_send_state {
    EPT_SEND_STATE_READY,
    EPT_SEND_STATE_SENDING,
};

struct backend_endpoint {
    struct ipc_ept_cfg cfg;
    bool is_registered;
    bool hold_rx_buf;
    uint8_t *rx_buffer;
    size_t rx_buf_size;
    size_t bytes_received;
    struct k_work_delayable rx_timeout_work;
};

struct backend_data {
    struct backend_endpoint endpoint;
    bool is_opened;
    struct k_mem_slab rx_slab;
    k_timeout_t rx_timeout;
    enum ept_send_state send_state;
    uint8_t *tx_buffer;
    struct k_work free_tx_work;
};

struct backend_config {
    const struct device *uart_dev;
    int64_t rx_timeout_usec;
};

static void endpoint_rx_timeout_handler(struct k_work *work) {
    struct backend_endpoint *ept = CONTAINER_OF(work, struct backend_endpoint, rx_timeout_work);
    unsigned int key = irq_lock();  // Prevent RX from writing to buffer after it is freed
    k_free(ept->rx_buffer);
    ept->rx_buffer = NULL;
    ept->bytes_received = 0;
    ept->rx_buf_size = 0;
    irq_unlock(key);

    if (ept->cfg.cb.error) {
        ept->cfg.cb.error("Transfer timed out waiting for next frame", ept->cfg.priv);
    }
}

static int register_endpoint(const struct device *instance, void **token, const struct ipc_ept_cfg *cfg) {
    if (instance == NULL || token == NULL || cfg == NULL) {
        LOG_ERR("One or more arguments are NULL");
        return -EINVAL;
    }

    struct backend_data *data = instance->data;

    if (data->endpoint.is_registered) {
        LOG_ERR("Endpoint \"%s\" already registered", data->endpoint.cfg.name);
        return -EALREADY;
    }

    data->endpoint.cfg = *cfg;
    char *name_buf = k_malloc(strlen(cfg->name) + 1);
    if (name_buf == NULL) {
        LOG_ERR("Failed to allocate memory for endpoint name");
        return -ENOMEM;
    }
    strcpy(name_buf, cfg->name);
    data->endpoint.cfg.name = name_buf;
    data->endpoint.is_registered = true;

    if (data->endpoint.cfg.cb.bound != NULL) {
        data->endpoint.cfg.cb.bound(data->endpoint.cfg.priv);
    }

    k_work_init_delayable(&data->endpoint.rx_timeout_work, endpoint_rx_timeout_handler);

    *token = &data->endpoint;
    return 0;
}

static int open_instance(const struct device *instance) {
    const struct backend_config *config = instance->config;
    struct backend_data *data = instance->data;

    const struct device *uart_dev = config->uart_dev;

    if (data->is_opened) {
        return -EALREADY;
    }

    int err = 0;

    uint8_t *rx_slab_buffer = k_malloc(2 * sizeof(struct uart_ipc_frame));
    if (rx_slab_buffer == NULL) {
        LOG_ERR("Failed to allocate memory for rx slab buffer %d", err);
        err = -ENOMEM;
        goto rx_slab_buffer_alloc_failed;
    }

    err = k_mem_slab_init(&data->rx_slab, rx_slab_buffer, ROUND_UP(sizeof(struct uart_ipc_frame), 4), 2);
    if (err) {
        LOG_ERR("Failed to initialize rx slab %d", err);
        goto rx_slab_init_failed;
    };

    void *initial_buf = NULL;
    err = k_mem_slab_alloc(&(data->rx_slab), &initial_buf, K_NO_WAIT);
    if (err) {
        LOG_ERR("Failed to allocate initial buffer from rx slab %d", err);
        goto init_buf_alloc_failed;
    }

    err = uart_callback_set(uart_dev, uart_callback, (void *)instance);
    if (err) {
        LOG_ERR("Failed to set uart callback %d", err);
        goto callback_set_failed;
    }

    err = uart_rx_enable(uart_dev, initial_buf, data->rx_slab.block_size, 100);
    LOG_DBG("Set initial rx buffer <%p>. Size: %d bytes ", initial_buf, data->rx_slab.block_size);
    if (err == -EBUSY) {
        err = -EALREADY;
    }

    if (err) {
        LOG_ERR("Failed to enable uart rx %d", err);
        goto rx_enable_failed;
    }

    data->is_opened = true;
    data->send_state = EPT_SEND_STATE_READY;
    return 0;

// Cleanup in case of failure
rx_enable_failed:
callback_set_failed:
    k_mem_slab_free(&data->rx_slab, &initial_buf);
init_buf_alloc_failed:
rx_slab_init_failed:
    k_free((void *)rx_slab_buffer);
rx_slab_buffer_alloc_failed:
    return err;
}

/**
 * @brief Packages the data into an array of frames. The returned array is suitable for passing to uart_tx.
 * The array is heap allocated and must eventually be freed.
 *
 * @param data Data to be packaged
 * @param len Length of the data
 * @param n_frames Number of frames in the returned array
 * @return struct uart_ipc_frame*
 */
static struct uart_ipc_frame *create_frames(const void *data, uint16_t len, size_t *n_frames) {
    uint16_t max_frag_size = (uint8_t)sizeof(((struct uart_ipc_frame *)0)->frag);
    uint16_t num_frames = (uint16_t)(len / max_frag_size);
    if (len % max_frag_size != 0) {
        num_frames++;
    }
    struct uart_ipc_frame *frames = k_calloc(num_frames, sizeof(struct uart_ipc_frame));
    if (frames == NULL) {
        LOG_ERR("Failed to allocate %d bytes for %d frames", num_frames * sizeof(struct uart_ipc_frame), num_frames);

        return NULL;
    }

    for (int i = 0; i < num_frames; ++i) {
        uint16_t frag_start = i * max_frag_size;
        uint8_t frag_len = MIN(max_frag_size, len - frag_start);

        frames[i].total_data_length = sys_cpu_to_le16(len);
        frames[i].frag_start = sys_cpu_to_le16(frag_start);
        frames[i].frag_len = frag_len;
        memcpy(frames[i].frag, (uint8_t *)data + frag_start, frag_len);
        frames[i].crc = sys_cpu_to_le32(crc32_ieee((uint8_t *)&frames[i], sizeof(frames[i]) - sizeof(frames[i].crc)));  // TODO: Calculate CRC
    }

    *n_frames = num_frames;

    return frames;
}

/**
 * @brief Extracts data from a frame into a buffer. The buffer must be large enough to hold the complete
 *        data from the transaction.
 *
 * @param dest_buf Buffer to hold received data
 * @param dest_buf_len Total size of the destination buffer
 * @param frame Frame to be unwrapped
 * @param added_data_len Number of bytes added to the destination buffer. Does not account for overlapping frames or preexisting data.
 * @return 0 on success, negative errno on failure: -EINVAL if crc check fails. -ENOMEM if the fragment would overflow the destination buffer.
 */
static int unwrap_frame(void *dest_buf, size_t dest_buf_len, struct uart_ipc_frame *frame, size_t *added_data_len) {
    uint32_t crc = crc32_ieee((uint8_t *)frame, sizeof(*frame) - sizeof(frame->crc));

    struct uart_ipc_frame_header {
        uint16_t total_data_length;
        uint16_t frag_start;
        uint8_t frag_len;
        uint32_t crc;
    } frame_hdr = { // Frame converted to CPU endianness
        .total_data_length = sys_le16_to_cpu(frame->total_data_length),
        .frag_start = sys_le16_to_cpu(frame->frag_start),
        .frag_len = frame->frag_len,
        .crc = sys_le32_to_cpu(frame->crc),
    };

    if (crc != frame_hdr.crc) {
        LOG_ERR("CRC mismatch. Fragment is likely corrupted");
        return -EINVAL;
    }

    if (frame_hdr.frag_start + frame_hdr.frag_len > dest_buf_len) {
        LOG_ERR("Frame overflows destination buffer");
        return -ENOMEM;
    }

    /*Uses frame instead of sys_end_frame to save on*/
    memcpy((uint8_t *)dest_buf + frame_hdr.frag_start, frame->frag, (size_t) frame_hdr.frag_len); 

    *added_data_len = frame_hdr.frag_len;
    return 0;
}

static int send(const struct device *instance, void *token, const void *data, size_t len) {
    struct backend_data *instance_data = instance->data;
    const struct backend_config *instance_config = instance->config;
    struct backend_endpoint *endpoint = (struct backend_endpoint *)token;

    if (instance_data->send_state == EPT_SEND_STATE_SENDING) {
        return -EBUSY;
    }

    size_t n_frames;
    struct uart_ipc_frame *frames = create_frames(data, len, &n_frames);
    if (frames == NULL) {
        if (endpoint->cfg.cb.error != NULL) {
            endpoint->cfg.cb.error("Could not allocate memory for data frames", endpoint->cfg.priv);
        }
        return -ENOMEM;
    }
    int err = uart_tx(instance_config->uart_dev, (void *)frames, n_frames * sizeof(struct uart_ipc_frame), SYS_FOREVER_US);
    if (err) {
        LOG_ERR("UART TX failed %d", err);
        if (endpoint->cfg.cb.error != NULL) {
            endpoint->cfg.cb.error("UART TX failed", endpoint->cfg.priv);
        }
        k_free((void *)frames);
        return err;
    }
    instance_data->tx_buffer = (uint8_t *)frames;
    return 0;
}

static void free_tx_work_handler(struct k_work *work_item) {
    LOG_DBG("Freeing tx buffer");
    struct backend_data *instance_data = CONTAINER_OF(work_item, struct backend_data, free_tx_work);
    k_free((void *)instance_data->tx_buffer);
    instance_data->send_state = EPT_SEND_STATE_READY;
}

const static struct ipc_service_backend backend_ops = {
    .open_instance = open_instance,
    .register_endpoint = register_endpoint,
    .send = send,
};

static int backend_init(const struct device *dev) {
    const struct backend_config *config = dev->config;
    struct backend_data *data = dev->data;

    if (!device_is_ready(config->uart_dev)) {
        LOG_ERR("UART device %s is not ready", config->uart_dev->name);
        return -ENODEV;
    }
    data->rx_timeout = K_USEC(config->rx_timeout_usec);

    k_work_init(&data->free_tx_work, free_tx_work_handler);
    return 0;
}

static inline int receive_frame(struct backend_endpoint *endpoint, struct uart_ipc_frame *frame, k_timeout_t rx_timeout) {
    if (endpoint->cfg.cb.received == NULL) {
        LOG_INF("Received data but no receive callback registered");
        return -EINVAL;
    }

    if (!K_TIMEOUT_EQ(rx_timeout, K_FOREVER)) {
        k_work_cancel_delayable(&endpoint->rx_timeout_work); /* New frame received, cancel timeout */
    }

    if (endpoint->rx_buffer == NULL) {
        if (frame->frag_start != 0) {
            LOG_ERR("New buffer started, but fragment starts at byte %d", frame->frag_start);
            return -EINVAL;
        }
        endpoint->bytes_received = 0;
        endpoint->rx_buffer = k_malloc(frame->total_data_length);
        if (endpoint->rx_buffer == NULL) {
            LOG_ERR("Failed to allocate memory for rx buffer");
            return -ENOMEM;
        }
        endpoint->rx_buf_size = frame->total_data_length;
    }

    size_t fragment_size = 0;
    int err = unwrap_frame(endpoint->rx_buffer, endpoint->rx_buf_size, frame, &fragment_size);

    endpoint->bytes_received += fragment_size;

    if (endpoint->bytes_received == frame->total_data_length) {
        endpoint->cfg.cb.received(endpoint->rx_buffer, endpoint->bytes_received, endpoint->cfg.priv);
        if (!endpoint->hold_rx_buf) {
            k_free((void *)endpoint->rx_buffer);
        }
        endpoint->bytes_received = 0;
        endpoint->rx_buffer = NULL;
        return 0;
    }
    k_work_reschedule(&endpoint->rx_timeout_work, rx_timeout); /* Start timeout for next frame */
    return 0;
}

static void uart_callback(const struct device *uart_dev, struct uart_event *evt, void *user_data) {
    struct device *instance = (struct device *)user_data;
    struct backend_data *data = instance->data;
    struct backend_endpoint *endpoint = &data->endpoint;

    switch (evt->type) {
        case UART_TX_DONE: {
            LOG_DBG("UART_TX_DONE");
            k_work_submit(&data->free_tx_work);
            break;
        }
        case UART_TX_ABORTED: {
            if (endpoint->cfg.cb.error != NULL) {
                endpoint->cfg.cb.error("Sending data was aborted", endpoint->cfg.priv);
            }
            k_work_submit(&data->free_tx_work);
            LOG_DBG("UART_TX_ABORTED");
            break;
        }
        case UART_RX_RDY: {
            LOG_DBG("UART_RX_RDY");
            if (evt->data.rx.len != sizeof(struct uart_ipc_frame)) {
                LOG_ERR("Received data is not a valid frame");
                if (endpoint->cfg.cb.error != NULL) {
                    endpoint->cfg.cb.error("Received data is not a valid frame. IPC instance is in an invalid state", endpoint->cfg.priv);
                }
                break;
            }
            struct uart_ipc_frame *frame = (struct uart_ipc_frame *)evt->data.rx.buf;
            int err = receive_frame(endpoint, frame, data->rx_timeout);
            if (err && endpoint->cfg.cb.error != NULL) {
                endpoint->cfg.cb.error("Failed to receive frame", endpoint->cfg.priv);
            }
            break;
        }
        case UART_RX_BUF_REQUEST: {
            LOG_DBG("UART_RX_BUF_REQUEST");
            uint8_t *new_buf = NULL;
            int err = k_mem_slab_alloc(&data->rx_slab, (void **)&new_buf, K_NO_WAIT);
            if (err || new_buf == NULL) {
                LOG_ERR("Failed to allocate new buffer from rx slab: %d", err);
                if (endpoint->cfg.cb.error != NULL) {
                    endpoint->cfg.cb.error("Failed to allocate new buffer from rx slab. Receiving will be interrupted.", endpoint->cfg.priv);
                }
                break;
            }
            LOG_DBG("Provisioning buffer of size %d bytes at <%p>", data->rx_slab.block_size, new_buf);
            err = uart_rx_buf_rsp(uart_dev, new_buf, data->rx_slab.block_size);
            if (err) {
                LOG_ERR("Failed to respond to rx buffer request: %d", err);
                if (endpoint->cfg.cb.error != NULL) {
                    endpoint->cfg.cb.error("Failed to respond to rx buffer request. Receiving will be interrupted.", endpoint->cfg.priv);
                }
            }
            break;
        }
        case UART_RX_BUF_RELEASED: {
            LOG_DBG("UART_RX_BUF_RELEASED");
            k_mem_slab_free(&data->rx_slab, (void **)&evt->data.rx_buf.buf);
            LOG_DBG("Released buffer <%p>", evt->data.rx_buf.buf);
            break;
        }
        case UART_RX_DISABLED: {
            LOG_DBG("UART_RX_DISABLED");
            if (endpoint->cfg.cb.error != NULL) {
                endpoint->cfg.cb.error("Receiving was disabled, attempting to restart.", endpoint->cfg.priv);
            }
            break;
        }
        case UART_RX_STOPPED: {
            if (endpoint->cfg.cb.error != NULL) {
                endpoint->cfg.cb.error("Receiving was stopped", endpoint->cfg.priv);
            }
            LOG_DBG("UART_RX_STOPPED");
            break;
        }
        default:
            break;
    }
}

#define DEFINE_BACKEND_DEVICE(inst)                                \
    static struct backend_config backend_config_##inst = {         \
        .uart_dev = DEVICE_DT_GET(DT_INST_BUS(inst)),              \
        .rx_timeout_usec = DT_INST_PROP(inst, rx_timeout),         \
    };                                                             \
    static struct backend_data backend_data_##inst = {0};          \
    DEVICE_DT_INST_DEFINE(inst,                                    \
                          &backend_init,                           \
                          NULL,                                    \
                          &backend_data_##inst,                    \
                          &backend_config_##inst,                  \
                          POST_KERNEL,                             \
                          CONFIG_IPC_SERVICE_REG_BACKEND_PRIORITY, \
                          &backend_ops);

DT_INST_FOREACH_STATUS_OKAY(DEFINE_BACKEND_DEVICE);
