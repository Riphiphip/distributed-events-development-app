/*
 * Copyright (c) 2021 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <app_event_manager.h>
#include <event_manager_proxy.h>
#include <ipc/ipc_service.h>

#define MODULE main
#include <caf/events/module_state_event.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(MODULE);

void main(void) {
    const struct device* instance = DEVICE_DT_GET(DT_NODELABEL(uart_ipc_backend));
    if (!device_is_ready(instance)) {
        LOG_ERR("IPC service backend is not ready");
        return;
    }

    if (app_event_manager_init()) {
        LOG_ERR("Application Event Manager not initialized");
    } else {
    }
    int err = event_manager_proxy_add_remote(instance);
    if (err) {
        LOG_ERR("IPC service register endpoint failed with error %d", err);
        return;
    }

	module_set_state(MODULE_STATE_READY);
}
