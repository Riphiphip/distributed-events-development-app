#include "ping_event.h"

static void log_ping_event(const struct app_event_header *aeh) {
    const struct ping_event *event = cast_ping_event(aeh);

    APP_EVENT_MANAGER_LOG(aeh, "message: %s", event->message);
}

APP_EVENT_TYPE_DEFINE(ping_event, log_ping_event, NULL, APP_EVENT_FLAGS_CREATE());