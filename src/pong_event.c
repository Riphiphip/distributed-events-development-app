#include "pong_event.h"

static void log_pong_event(const struct app_event_header *aeh) {
    const struct pong_event *event = cast_pong_event(aeh);

    APP_EVENT_MANAGER_LOG(aeh, "message: %s", event->message);
}

APP_EVENT_TYPE_DEFINE(pong_event, log_pong_event, NULL, APP_EVENT_FLAGS_CREATE());