// Memory channels: 32 NVS-persisted frequency/mode/label slots.
// Full blob (~1 KB) written on every set/clear — user-action rate,
// so flash wear is negligible.
#include "mem_channels.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "mem_channels";
#define PART  "user_nvs"
#define NS    "qmx"
#define KEY   "mem_slots"

static mem_slot_t   s_slots[MEM_SLOTS];
static nvs_handle_t s_nvs  = 0;
static bool         s_ready = false;

void mem_channels_init(void)
{
    esp_err_t err = nvs_open_from_partition(PART, NS, NVS_READWRITE, &s_nvs);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open failed: 0x%x - channels will not persist", err);
        return;
    }
    size_t sz = sizeof(s_slots);
    err = nvs_get_blob(s_nvs, KEY, s_slots, &sz);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        memset(s_slots, 0, sizeof(s_slots));
        ESP_LOGI(TAG, "no saved channels - starting empty");
    } else if (err != ESP_OK) {
        memset(s_slots, 0, sizeof(s_slots));
        ESP_LOGW(TAG, "blob read error 0x%x - starting empty", err);
    } else {
        int n = 0;
        for (int i = 0; i < MEM_SLOTS; i++) n += s_slots[i].occupied ? 1 : 0;
        ESP_LOGI(TAG, "loaded %d/%d channels", n, MEM_SLOTS);
    }
    s_ready = true;
}

bool mem_channels_get(int idx, mem_slot_t *out)
{
    if (idx < 0 || idx >= MEM_SLOTS || !out) return false;
    *out = s_slots[idx];
    return true;
}

void mem_channels_set(int idx, const mem_slot_t *slot)
{
    if (idx < 0 || idx >= MEM_SLOTS || !slot || !s_ready) return;
    s_slots[idx] = *slot;
    esp_err_t err = nvs_set_blob(s_nvs, KEY, s_slots, sizeof(s_slots));
    if (err == ESP_OK) err = nvs_commit(s_nvs);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "save slot %d failed: 0x%x", idx, err);
    } else {
        ESP_LOGI(TAG, "slot %d saved: %lu Hz  %s  '%s'",
                 idx, (unsigned long)slot->freq_hz, slot->mode, slot->label);
    }
}

void mem_channels_clear(int idx)
{
    if (idx < 0 || idx >= MEM_SLOTS || !s_ready) return;
    memset(&s_slots[idx], 0, sizeof(mem_slot_t));
    esp_err_t err = nvs_set_blob(s_nvs, KEY, s_slots, sizeof(s_slots));
    if (err == ESP_OK) err = nvs_commit(s_nvs);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "clear slot %d failed: 0x%x", idx, err);
    } else {
        ESP_LOGI(TAG, "slot %d cleared", idx);
    }
}
