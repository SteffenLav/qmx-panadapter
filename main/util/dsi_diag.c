// dsi_diag.c - temporary diagnostic poller for the FT4 full-screen cyan flash
// (2026-07-13). The DSI bridge underrun IRQ has never fired during a flash, so
// the framebuffer feed looks healthy - this watches the layer below: the DSI
// HOST's error status (D-PHY errors, panel-reported ACK errors, ECC/CRC/
// timeout). INT_ST0/INT_ST1 are read-to-clear and their interrupts are not
// used by the esp_lcd driver, so polling them from a task is safe and doesn't
// race anything. On any nonzero status a WARN line with both raw words lands
// in the diag ring, timestamped, so a visual flash can be matched against a
// register fingerprint (or its absence - which would point at the panel
// itself, not the link).
//
// Remove this module once the flash is root-caused.

#include "dsi_diag.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "soc/mipi_dsi_host_struct.h"

#include "psram_task.h"

static const char *TAG = "dsidiag";

#define DSI_DIAG_POLL_MS 10

// The ST7123 link shows a CONSTANT dphy_errors_4 (int_st0 bit 20, LP-lane
// contention) at essentially every poll - hundreds of hits per second, present
// from boot, clearly a standing condition of this panel/link and not the
// 2-minute flash event. Count it silently; only log when any OTHER bit
// appears, plus a 10 s summary so the baseline noise rate stays visible.
#define DSI_ST0_CONSTANT_NOISE  (1u << 20)

static void dsi_diag_task(void *arg)
{
    (void)arg;
    uint32_t noise_hits = 0;
    uint32_t polls = 0;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(DSI_DIAG_POLL_MS));
        polls++;
        uint32_t st0 = MIPI_DSI_HOST.int_st0.val;  // read clears
        uint32_t st1 = MIPI_DSI_HOST.int_st1.val;  // read clears
        if (st0 & DSI_ST0_CONSTANT_NOISE) noise_hits++;
        st0 &= ~DSI_ST0_CONSTANT_NOISE;
        if (st0 || st1) {
            // st0: [15:0] ack_with_err (panel-reported), [20:16] dphy_errors
            // st1: [0] to_hs_tx [1] to_lp_rx [2] ecc_single [3] ecc_multi
            //      [4] crc_err [5] pkt_size_err [6] eopt_err ...
            ESP_LOGW(TAG, "DSI host error: int_st0=0x%08lx int_st1=0x%08lx",
                     (unsigned long)st0, (unsigned long)st1);
        }
        if (polls >= 10000 / DSI_DIAG_POLL_MS) {  // every 10 s
            ESP_LOGI(TAG, "baseline: dphy_err4 %lu/%lu polls",
                     (unsigned long)noise_hits, (unsigned long)polls);
            noise_hits = 0;
            polls = 0;
        }
    }
}

void dsi_diag_start(void)
{
    if (psram_task_create(dsi_diag_task, "dsi_diag", 3072, NULL, 1, tskNO_AFFINITY)) {
        ESP_LOGI(TAG, "DSI host error poller running (%d ms)", DSI_DIAG_POLL_MS);
    } else {
        ESP_LOGE(TAG, "poller task create failed");
    }
}
