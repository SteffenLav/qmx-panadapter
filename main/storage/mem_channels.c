// Memory channels: 32 NVS-persisted frequency/mode/label slots.
// Full blob (~1 KB) written on every set/clear — user-action rate,
// so flash wear is negligible.
#include "mem_channels.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "mem_channels";
#define PART       "user_nvs"
#define NS         "qmx"
#define KEY        "mem_slots"
#define KEY_SEEDED "mem_seeded"
#define KEY_DEMO   "mem_demo_shown"

static mem_slot_t   s_slots[MEM_SLOTS];
static nvs_handle_t s_nvs  = 0;
static bool         s_ready = false;

// Factory-default channels shipped from the developer's own working set
// (slots 13/14/15/16, 1-based as shown in the UI -> 0-based idx 12-15
// here), so first-time users land on a populated, explorable grid instead
// of 32 blank cells. Applied at most ONCE ever per device (see
// KEY_SEEDED below) and only into whichever of these 4 slots are still
// empty - never overwrites a slot the user (or an earlier firmware) has
// already put something in.
typedef struct { int idx; uint32_t freq_hz; const char *mode; const char *label; } mem_default_t;
static const mem_default_t DEFAULT_SLOTS[] = {
    { 12, 14080000, "DiGi", "Steffen" },
    { 13, 45856000, "USB",  "Pia"     },
    { 14,  7074000, "LSB",  "Johane"  },
    { 15,  3560000, "CW",   "Astrid"  },
};

static void seed_defaults_if_needed(void)
{
    uint8_t seeded = 0;
    if (nvs_get_u8(s_nvs, KEY_SEEDED, &seeded) == ESP_OK && seeded) return;  // already ran, ever

    int n_applied = 0;
    for (size_t i = 0; i < sizeof(DEFAULT_SLOTS) / sizeof(DEFAULT_SLOTS[0]); i++) {
        const mem_default_t *d = &DEFAULT_SLOTS[i];
        if (s_slots[d->idx].occupied) continue;   // don't clobber existing data
        mem_slot_t slot = { 0 };
        slot.freq_hz = d->freq_hz;
        strncpy(slot.mode, d->mode, sizeof(slot.mode) - 1);
        strncpy(slot.label, d->label, sizeof(slot.label) - 1);
        slot.occupied = 1;
        s_slots[d->idx] = slot;
        n_applied++;
    }
    if (n_applied > 0) {
        esp_err_t werr = nvs_set_blob(s_nvs, KEY, s_slots, sizeof(s_slots));
        if (werr == ESP_OK) werr = nvs_commit(s_nvs);
        if (werr != ESP_OK) ESP_LOGW(TAG, "default-slot save failed: 0x%x", werr);
    }
    ESP_LOGI(TAG, "seeded %d/%d default memory channel(s)",
             n_applied, (int)(sizeof(DEFAULT_SLOTS) / sizeof(DEFAULT_SLOTS[0])));

    // Mark done regardless of how many were actually applied (even 0) - this
    // must never re-check on a later boot, or a slot the user deliberately
    // cleared afterward would get silently re-seeded.
    nvs_set_u8(s_nvs, KEY_SEEDED, 1);
    nvs_commit(s_nvs);
}

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
    seed_defaults_if_needed();
}

bool mem_channels_demo_shown(void)
{
    if (!s_ready) return true;  // NVS unavailable - default to "shown" so it can't wedge into repeating
    uint8_t v = 0;
    return nvs_get_u8(s_nvs, KEY_DEMO, &v) == ESP_OK && v != 0;
}

void mem_channels_mark_demo_shown(void)
{
    if (!s_ready) return;
    nvs_set_u8(s_nvs, KEY_DEMO, 1);
    nvs_commit(s_nvs);
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
