#include <app_event_manager.h>

struct shared_event {
    struct app_event_header header;
    int value;
};

APP_EVENT_TYPE_DECLARE(shared_event);