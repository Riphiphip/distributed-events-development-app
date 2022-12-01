#include <app_event_manager.h>

struct pong_event {
    struct app_event_header header;
    char message[128];
    uint8_t counter;
};

APP_EVENT_TYPE_DECLARE(pong_event);