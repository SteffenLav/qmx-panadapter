// ft8_hash.c - FT8 callsign hash table. See ft8_hash.h for the full story.

#include "ft8_hash.h"

#include <string.h>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "ft8/text.h"   // nchar() + FT8_CHAR_TABLE_ALPHANUM_SPACE_SLASH

static const char *TAG = "ft8_hash";

// 256 recent calls is plenty: the decode table itself (ft8_screen) holds far
// fewer live stations, and entries are only needed while the station is still
// being worked/heard. Round-robin eviction recycles the oldest inserts.
#define FT8_HASH_TABLE_SIZE 256

typedef struct {
    char     call[12];   // full callsign, 11 chars max (FT8 c58 limit) + NUL
    uint32_t n22;        // full 22-bit hash; n12 = n22 >> 10, n10 = n22 >> 12
} ft8_hash_entry_t;

static ft8_hash_entry_t *s_tab;         // PSRAM, FT8_HASH_TABLE_SIZE entries
static int               s_next;        // round-robin insert cursor
static SemaphoreHandle_t s_mutex;

// ---------------------------------------------------------------------------
// The 22-bit callsign hash - same algorithm as ft8_lib's (static) save_callsign
// in message.c: base-38 over the callsign padded to 11 chars with trailing
// spaces, then the standard FT8 multiplicative hash. Replicated here (a) so
// ft8_hash_seed() can add OUR OWN call without a dummy encode round-trip, and
// (b) so lookups can compare against the stored full-width hash regardless of
// which truncation (22/12/10 bits) the wire carried.
// ---------------------------------------------------------------------------
static bool call_to_n22(const char *callsign, uint32_t *n22_out)
{
    uint64_t n58 = 0;
    int i = 0;
    while (callsign[i] != '\0' && i < 11) {
        int j = nchar(callsign[i], FT8_CHAR_TABLE_ALPHANUM_SPACE_SLASH);
        if (j < 0) return false;   // character outside the callsign set
        n58 = (38 * n58) + (uint64_t)j;
        i++;
    }
    while (i < 11) {   // pad with trailing spaces (index 0)
        n58 = 38 * n58;
        i++;
    }
    *n22_out = (uint32_t)((47055833459ull * n58) >> (64 - 22)) & 0x3FFFFFul;
    return true;
}

// ftx_callsign_hash_interface_t.save_hash - called by ft8_lib for every full
// callsign it packs (encode) or unpacks (decode), with the ready-made n22.
static void hash_save(const char *callsign, uint32_t n22)
{
    if (!s_tab || !callsign || !callsign[0]) return;
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return;

    // Already present? (Same call always hashes the same, so just return.)
    for (int i = 0; i < FT8_HASH_TABLE_SIZE; i++) {
        if (s_tab[i].call[0] && strncmp(s_tab[i].call, callsign, 11) == 0) {
            xSemaphoreGive(s_mutex);
            return;
        }
    }
    ft8_hash_entry_t *e = &s_tab[s_next];
    s_next = (s_next + 1) % FT8_HASH_TABLE_SIZE;
    strncpy(e->call, callsign, 11);
    e->call[11] = '\0';
    e->n22 = n22;
    xSemaphoreGive(s_mutex);
}

// ftx_callsign_hash_interface_t.lookup_hash - resolve a wire hash back to a
// full callsign. 12- and 10-bit hashes are the TOP bits of n22 (n22 >> 10 /
// n22 >> 12 - see ft8_lib's save_callsign), so compare against the stored
// n22 shifted accordingly. Plain linear scan; see ft8_hash.h for why the
// reference implementation's hashed probe is broken for the short types.
static bool hash_lookup(ftx_callsign_hash_type_t hash_type, uint32_t hash, char *callsign)
{
    callsign[0] = '\0';
    if (!s_tab) return false;
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return false;

    bool found = false;
    for (int i = 0; i < FT8_HASH_TABLE_SIZE; i++) {
        if (!s_tab[i].call[0]) continue;
        uint32_t h = s_tab[i].n22;
        if (hash_type == FTX_CALLSIGN_HASH_12_BITS)      h >>= 10;
        else if (hash_type == FTX_CALLSIGN_HASH_10_BITS) h >>= 12;
        if (h == hash) {
            strcpy(callsign, s_tab[i].call);   // <= 11 chars + NUL, caller has 12
            found = true;
            break;
        }
    }
    xSemaphoreGive(s_mutex);
    return found;
}

static ftx_callsign_hash_interface_t s_if = {
    .lookup_hash = hash_lookup,
    .save_hash   = hash_save,
};

void ft8_hash_init(void)
{
    if (s_tab) return;   // idempotent
    SemaphoreHandle_t m = xSemaphoreCreateMutex();
    if (!m) {
        ESP_LOGE(TAG, "mutex alloc failed - hash table disabled");
        return;
    }
    ft8_hash_entry_t *t = heap_caps_calloc(FT8_HASH_TABLE_SIZE, sizeof(ft8_hash_entry_t),
                                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!t) {
        ESP_LOGE(TAG, "table alloc failed - hash table disabled");
        vSemaphoreDelete(m);
        return;
    }
    s_mutex = m;
    s_next  = 0;
    s_tab   = t;   // publish LAST - ft8_hash_if() gates on s_tab
    ESP_LOGI(TAG, "callsign hash table ready (%d entries, PSRAM)", FT8_HASH_TABLE_SIZE);
}

ftx_callsign_hash_interface_t *ft8_hash_if(void)
{
    return s_tab ? &s_if : NULL;
}

void ft8_hash_seed(const char *callsign)
{
    if (!s_tab || !callsign || !callsign[0]) return;
    uint32_t n22;
    if (!call_to_n22(callsign, &n22)) {
        ESP_LOGW(TAG, "seed: '%s' has characters outside the callsign set", callsign);
        return;
    }
    hash_save(callsign, n22);
    ESP_LOGI(TAG, "seeded own call '%s' (n22=%lu)", callsign, (unsigned long)n22);
}

// ---------------------------------------------------------------------------
// Boot self-test: round-trip a nonstandard-call exchange the way it arrives
// on the air. Small stack footprint (a few hundred bytes) - safe inline.
// ---------------------------------------------------------------------------
bool ft8_hash_selftest(void)
{
    ft8_hash_init();
    if (!ft8_hash_if()) {
        ESP_LOGE(TAG, "selftest: init failed");
        return false;
    }

    bool ok = true;
    char text[FTX_MAX_MESSAGE_LENGTH];
    ftx_message_t msg;
    ftx_message_offsets_t off;

    // 1) Our own call seeded, then a special-call station answers our CQ:
    //    on the wire that's i3=4 with OUR call as a 12-bit hash and their
    //    full nonstandard call in c58. It must decode with our call resolved.
    ft8_hash_seed("K1ABC");
    if (ftx_message_encode(&msg, ft8_hash_if(), "K1ABC PJ4/K9XYZ RR73") != FTX_MESSAGE_RC_OK) {
        ESP_LOGE(TAG, "selftest: encode nonstd answer failed");
        ok = false;
    } else if (ftx_message_decode(&msg, ft8_hash_if(), text, &off) != FTX_MESSAGE_RC_OK) {
        ESP_LOGE(TAG, "selftest: decode nonstd answer failed");
        ok = false;
    } else if (!strstr(text, "K1ABC") || !strstr(text, "PJ4/K9XYZ")) {
        ESP_LOGE(TAG, "selftest: hash didn't resolve: '%s'", text);
        ok = false;
    } else {
        ESP_LOGI(TAG, "selftest: nonstd answer round-trip OK: '%s'", text);
    }

    // 2) 22-bit path: a std message whose FIRST call is a hashed nonstandard
    //    call (our reply TO the special station). PJ4/K9XYZ was saved by the
    //    encode above, so it must resolve here.
    if (ok) {
        if (ftx_message_encode_std(&msg, ft8_hash_if(), "PJ4/K9XYZ", "K1ABC", "FN42") != FTX_MESSAGE_RC_OK) {
            ESP_LOGE(TAG, "selftest: encode hashed-target reply failed");
            ok = false;
        } else if (ftx_message_decode(&msg, ft8_hash_if(), text, &off) != FTX_MESSAGE_RC_OK) {
            ESP_LOGE(TAG, "selftest: decode hashed-target reply failed");
            ok = false;
        } else if (!strstr(text, "PJ4/K9XYZ") || !strstr(text, "K1ABC")) {
            ESP_LOGE(TAG, "selftest: 22-bit hash didn't resolve: '%s'", text);
            ok = false;
        } else {
            ESP_LOGI(TAG, "selftest: hashed-target reply round-trip OK: '%s'", text);
        }
    }

    ESP_LOGI(TAG, "selftest: %s", ok ? "PASS" : "FAIL");
    return ok;
}
