#include "shared_event.h"

static void log_shared_event(const struct app_event_header *aeh) {
    struct shared_event *event = cast_shared_event(aeh);

    APP_EVENT_MANAGER_LOG(aeh, "value: %d", event->value);
}

APP_EVENT_TYPE_DEFINE(shared_event, log_shared_event, NULL, APP_EVENT_FLAGS_CREATE());