#include <zephyr/fff.h>

#include <zephyr/ipc/ipc_service.h>

#include "C:/Work/distributed_events_proposal/distributed_events/drivers/zephyr,uart-ipc-service-backend.c"

DEFINE_FFF_GLOBALS;

int test_main(void)
{
    uart_callback(0, 0, 0);
    return 0;
}