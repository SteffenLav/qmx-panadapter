#include "gpio_relay.h"

#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_log.h"

#include <string.h>

#include "storage/settings.h"
#include <stdio.h>

static const char *TAG = "gpio_relay";

#define RELAY_PIN_A GPIO_NUM_53
#define RELAY_PIN_B GPIO_NUM_54

static esp_timer_handle_t s_release_timer;
static volatile bool      s_busy = false;
static bool               s_inited = false;   /* settings can be set before init() runs */
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

    /* ⛔ RESTING LEVEL IS THE STORED POLARITY'S INACTIVE SIDE, NOT A FIXED LOW.
     * This drove both pins LOW unconditionally, under a comment reasoning that
     * "a contact closure should require a deliberate pulse". That reasoning is
     * right and the code did the opposite of it for anyone who had chosen
     * active LOW: LOW IS THE ASSERTED STATE for them, so the relay was held
     * CLOSED from boot until the first pulse happened to release it - on a line
     * wired to a QMX's PWR_ON. Randy N4OPI found it and put it mildly ("doesn't
     * really matter much except if you have chosen an active setting of Low").
     *
     * Both pins are set, not just the configured one: the polarity the operator
     * declared is a fact about their wiring, so a later pin change must not
     * leave the other pin resting on the wrong side. */
    uint8_t  st_pin = 53;
    bool     st_level = true;
    uint16_t st_ms = 1000;
    settings_get_gpio_relay(&st_pin, &st_level, &st_ms);   /* narrow accessor - never settings_load_all() here */
    s_rest_level = !st_level;
    gpio_set_level(RELAY_PIN_A, s_rest_level ? 1 : 0);
    gpio_set_level(RELAY_PIN_B, s_rest_level ? 1 : 0);

    const esp_timer_create_args_t targs = {
        .callback = release_cb,
        .name     = "gpio_relay_release",
    };
    esp_timer_create(&targs, &s_release_timer);

    s_inited = true;
    ESP_LOGI(TAG, "GPIO53/54 configured as outputs, active %s so resting %s (%s)",
             st_level ? "HIGH" : "LOW", s_rest_level ? "HIGH" : "LOW",
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

void gpio_relay_set_polarity(bool active_level)
{
    bool rest = !active_level;

    /* Mid-pulse, only the destination changes: driving the pin now would cut
     * the pulse short, and the pulse is a real contact closure on someone's
     * radio. release_cb lands on the new resting level when the timer fires. */
    s_rest_level = rest;
    /* A setting can be written before init() has configured the pins (config
     * import, say). The stored resting level above is what init() will read,
     * so there is nothing to drive yet. */
    if (!s_inited) return;
    if (s_busy) {
        ESP_LOGI(TAG, "polarity now active %s - resting level applies when the pulse ends",
                 active_level ? "HIGH" : "LOW");
        return;
    }

    gpio_set_level(RELAY_PIN_A, rest ? 1 : 0);
    gpio_set_level(RELAY_PIN_B, rest ? 1 : 0);
    ESP_LOGI(TAG, "polarity now active %s - both pins resting %s",
             active_level ? "HIGH" : "LOW", rest ? "HIGH" : "LOW");
}
