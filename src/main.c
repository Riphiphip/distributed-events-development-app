/*
 * Copyright (c) 2021 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <app_event_manager.h>
#include <event_manager_proxy.h>
#include <ipc/ipc_service.h>

#include "ping_event.h"
#include "pong_event.h"

#define MODULE APPLICATION

#ifdef CONFIG_PING
#define SUBSCRIBE_TO_EVENT pong_event
#endif

#ifdef CONFIG_PONG
#define SUBSCRIBE_TO_EVENT ping_event
#endif

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(MODULE, CONFIG_APPLICATION_LOG_LEVEL);

void main(void) {
    const struct device *instance = DEVICE_DT_GET(DT_NODELABEL(uart_ipc_backend));
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

    err = EVENT_MANAGER_PROXY_SUBSCRIBE(instance, SUBSCRIBE_TO_EVENT);

    if (err) {
        LOG_ERR("Proxy subscribe failed with error %d", err);
        return;
    }

    err = event_manager_proxy_start();
    if (err) {
        LOG_ERR("IPC service start failed with error %d", err);
        return;
    }
    event_manager_proxy_wait_for_remotes(K_FOREVER);
    k_sleep(K_MSEC(200));

#ifdef CONFIG_PING
    LOG_DBG("Sending first ping event!");
    struct ping_event *event = new_ping_event();
    strncpy(&event->message[0], "This is the first PING event!", sizeof(event->message));
    event->counter = 0;
    APP_EVENT_SUBMIT(event);
#endif /* CONFIG_PING */
}

static bool app_event_handler(const struct app_event_header *aeh) {
    if (is_ping_event(aeh)) {
        const struct ping_event *event = cast_ping_event(aeh);
        LOG_INF("PING! (%d) : %s",event->counter, event->message);
        // k_sleep(K_MSEC(500));
        struct pong_event *pong = new_pong_event();
        strncpy(&pong->message[0], "Hello from the PONG server!", sizeof(pong->message));
        pong->counter = event->counter + 1;
        APP_EVENT_SUBMIT(pong);
        LOG_INF("Sending PONG!");
        return false;
    }

    if (is_pong_event(aeh)) {
        const struct pong_event *event = cast_pong_event(aeh);
        LOG_INF("PONG! (%d) : %s",event->counter, event->message);
        // k_sleep(K_MSEC(500));
        struct ping_event *ping = new_ping_event();
        strncpy(&ping->message[0], "Hello from the PING client!", sizeof(ping->message));
        ping->counter = event->counter + 1;
        APP_EVENT_SUBMIT(ping);
        LOG_INF("Sending PING!");
        return false;
    }

    return false;
}

APP_EVENT_LISTENER(MODULE, app_event_handler);
APP_EVENT_SUBSCRIBE(MODULE, SUBSCRIBE_TO_EVENT);
