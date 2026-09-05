#include "gpio_relay.h"

#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_log.h"

#include <string.h>
#include <stdio.h>

static const char *TAG = "gpio_relay";

#define RELAY_PIN_A GPIO_NUM_53
#define RELAY_PIN_B GPIO_NUM_54

static esp_timer_handle_t s_release_timer;
static volatile bool      s_busy = false;
static uint8_t            s_active_pin;
static bool                s_rest_level;   /* level to return to when the timer fires */

static gpio_num_t pin_to_gpio(uint8_t pin)
{
    if (pin == 53) return RELAY_PIN_A;
    if (pin == 54) return RELAY_PIN_B;
    return GPIO_NUM_NC;
}

static void release_cb(void *arg)
{
    (void)arg;
    gpio_set_level((gpio_num_t)pin_to_gpio(s_active_pin), s_rest_level ? 1 : 0);
    ESP_LOGI(TAG, "GPIO%u released -> %d", s_active_pin, (int)s_rest_level);
    s_busy = false;
}

void gpio_relay_init(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << RELAY_PIN_A) | (1ULL << RELAY_PIN_B),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    esp_err_t e = gpio_config(&cfg);
    /* Resting LOW on both - see the header: a contact closure should require
     * a deliberate pulse, never float or default to closed. */
    gpio_set_level(RELAY_PIN_A, 0);
    gpio_set_level(RELAY_PIN_B, 0);

    const esp_timer_create_args_t targs = {
        .callback = release_cb,
        .name     = "gpio_relay_release",
    };
    esp_timer_create(&targs, &s_release_timer);

    ESP_LOGI(TAG, "GPIO53/54 configured as outputs, resting LOW (%s)",
             e == ESP_OK ? "ok" : "gpio_config FAILED");
}

bool gpio_relay_pulse(uint8_t pin, bool level, uint16_t ms, char *err, size_t errlen)
{
    gpio_num_t g = pin_to_gpio(pin);
    if (g == GPIO_NUM_NC) {
        if (err) snprintf(err, errlen, "pin must be 53 or 54");
        return false;
    }
    if (ms < GPIO_RELAY_MIN_MS || ms > GPIO_RELAY_MAX_MS) {
        if (err) snprintf(err, errlen, "duration must be %u-%u ms",
                           (unsigned)GPIO_RELAY_MIN_MS, (unsigned)GPIO_RELAY_MAX_MS);
        return false;
    }
    if (s_busy) {
        if (err) snprintf(err, errlen, "a pulse is already in progress");
        return false;
    }

    s_busy        = true;
    s_active_pin  = pin;
    s_rest_level  = !level;
    gpio_set_level(g, level ? 1 : 0);
    esp_timer_start_once(s_release_timer, (uint64_t)ms * 1000ULL);

    ESP_LOGW(TAG, "GPIO%u -> %d for %u ms (relay pulse)", pin, (int)level, (unsigned)ms);
    return true;
}

bool gpio_relay_busy(void) { return s_busy; }
