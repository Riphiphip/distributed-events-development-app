#include <caf/events/button_event.h>
#include <caf/gpio_pins.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>

#define MODULE LED_MODULE
#include <caf/events/module_state_event.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(MODULE);


struct gpio_output_pin_w_state {
    uint8_t port;
    uint8_t pin;
    bool state;
};

static bool app_event_handler(const struct app_event_header *evt) {
    LOG_DBG("Led module received event");
    const struct device *gpio0 = device_get_binding(DT_LABEL(DT_NODELABEL(gpio0)));
    struct gpio_output_pin_w_state led_gpios[] = {
        {.port = 0, .pin = 13, .state = 0},
        {.port = 0, .pin = 14, .state = 0},
        {.port = 0, .pin = 15, .state = 0},
        {.port = 0, .pin = 16, .state = 0},
    };

    if (is_button_event(evt)) {
        
        const struct button_event *event = cast_button_event(evt);
        if (event->key_id < 4) {
            LOG_DBG("Button %d pressed", event->key_id);
            gpio_pin_configure(gpio0, led_gpios[event->key_id].pin, GPIO_OUTPUT);
            gpio_pin_set(gpio0, led_gpios[event->key_id].pin, !led_gpios[event->key_id].state);
            led_gpios[event->key_id].state = !led_gpios[event->key_id].state;
        }
    }

    if (is_module_state_event(evt)) {
        const struct module_state_event *event = cast_module_state_event(evt);
        if (check_state(event, MODULE_ID(main), MODULE_STATE_READY)) {
            module_set_state(MODULE_STATE_READY);
            LOG_DBG("Led module ready");
        }
    }

    return false;
}

APP_EVENT_LISTENER(MODULE, app_event_handler);
APP_EVENT_SUBSCRIBE(MODULE, module_state_event);
APP_EVENT_SUBSCRIBE(MODULE, button_event);