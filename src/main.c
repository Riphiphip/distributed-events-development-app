/*
 * Copyright (c) 2021 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <app_event_manager.h>
#include <event_manager_proxy.h>
#include <ipc/ipc_service.h>

#include "shared_event.h"

#define MODULE APPLICATION

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(MODULE, CONFIG_APPLICATION_LOG_LEVEL);

	void bound(void *priv){
        LOG_DBG("bound");
    }

	void received(const void *data, size_t len, void *priv){
        LOG_DBG("%.*s", len, (const char *) data);
    }

	void error(const char *message, void *priv){
        LOG_ERR("error: %s", message);
    }

void main(void) {
    const struct device* instance = DEVICE_DT_GET(DT_NODELABEL(uart_ipc_backend));
    if (!device_is_ready(instance)) {
        LOG_ERR("IPC service backend is not ready");
        return;
    }

    if (app_event_manager_init()) {
        LOG_ERR("Application Event Manager not initialized");
        return;
    }

    int err = event_manager_proxy_add_remote(instance);
    if (err) {
        LOG_ERR("IPC service register endpoint failed with error %d", err);
        return;
    }
    k_sleep(K_MSEC(1000));

#ifdef CONFIG_TRANSMIT
LOG_INF("I AM TRANSMITTER");
    err = event_manager_proxy_start();
    if (err) {
        LOG_ERR("IPC service start failed with error %d", err);
        return;
    }
    event_manager_proxy_wait_for_remotes(K_FOREVER);
    k_sleep(K_MSEC(200));
    int prev_value = 0;
    while (true) {
        struct shared_event* event = new_shared_event();
        event->value = !prev_value;
        prev_value = event->value;
        APP_EVENT_SUBMIT(event);
        k_sleep(K_MSEC(1000));
    }
#else
LOG_INF("I AM RECEIVER");
    err = EVENT_MANAGER_PROXY_SUBSCRIBE(instance, shared_event);
    if (err) {
        LOG_ERR("Remote subscribe failed with error %d", err);
        return;
    }
    err = event_manager_proxy_start();
    if (err) {
        LOG_ERR("Proxy start failed with error %d", err);
        return;
    }
    k_sleep(K_MSEC(200));
    event_manager_proxy_wait_for_remotes(K_FOREVER);
#endif /* CONFIG_TRANSMIT */
}

#ifndef CONFIG_TRANSMIT

static bool app_event_handler(const struct app_event_header *aeh)
{
        if (is_shared_event(aeh)) {

                /* Accessing event data. */
                const struct shared_event *event = cast_shared_event(aeh);
                    LOG_INF("Received event with value: %d", event->value);
                return false;
        }

        return false;
}

APP_EVENT_LISTENER(MODULE, app_event_handler);
APP_EVENT_SUBSCRIBE(MODULE, shared_event);

#endif /* CONFIG_TRANSMIT */