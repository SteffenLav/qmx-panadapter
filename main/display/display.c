#include "display.h"

#include <string.h>
#include "esp_log.h"
#include "esp_heap_caps.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "bsp/m5stack_tab5.h"
#include "bsp/display.h"
#include "esp_lvgl_port.h"
#include "hal/axi_icm_ll.h"
#include "esp_timer.h"
#include "nvs.h"

static const char *TAG = "display";

/* ---------------------------------------------------------------------------
 * FRAME COUNTER - so "smooth" is a NUMBER instead of a question to the operator.
 *
 * CLAUDE.md's module map lists a `util/fps.c`; that file does not exist, so
 * there has never been a frame-rate readout anywhere - not on screen, not in
 * /api/status, not in the log. Which meant the only instrument for the
 * panadapter's smoothness was asking the operator to look, and on 2026-08-28 he
 * gave the correct answer to that: *"these differences is super hard to see"*.
 * A ~2 percentage-point change in core-0 idle is real and invisible; asking eyes
 * to resolve it is asking the wrong instrument.
 *
 * LV_EVENT_REFR_READY fires once per completed LVGL refresh, which is exactly
 * the rate the operator perceives. O(1) per frame, so unlike the heap and stack
 * walks this is safe to leave on a periodic path (see the cyan-flash rule).
 * ------------------------------------------------------------------------- */
static volatile uint32_t s_frames;
/* Invalidated pixels, summed per LV_EVENT_INVALIDATE_AREA. Frames alone do NOT
 * characterise the load: with the radio off this display runs at 28 fps and
 * core-0 idle is ~57%, while with audio it runs at ~13 fps and idle is 4-7% -
 * i.e. the expensive variable is HOW MUCH is redrawn each frame, not how often.
 * ⚠ This over-counts where LVGL later merges overlapping areas, so read it as
 * "pixels requested", a load proxy, not an exact blit figure. O(1) per event. */
static volatile uint64_t s_inval_px;

static void disp_refr_ready_cb(lv_event_t *e)
{
    (void)e;
    s_frames++;
}

static void disp_inval_area_cb(lv_event_t *e)
{
    const lv_area_t *a = (const lv_area_t *)lv_event_get_param(e);
    if (!a) return;
    int32_t w = a->x2 - a->x1 + 1;
    int32_t h = a->y2 - a->y1 + 1;
    if (w > 0 && h > 0) s_inval_px += (uint64_t)w * (uint64_t)h;
}

// Thousands of invalidated pixels per second since the previous call.
unsigned display_inval_kpx_per_s(void)
{
    static uint64_t last_px;
    static int64_t  last_us;
    int64_t  now = esp_timer_get_time();
    uint64_t px  = s_inval_px;
    unsigned out = 0;
    if (last_us && now > last_us)
        out = (unsigned)(((px - last_px) * 1000000ULL) / (uint64_t)(now - last_us) / 1000ULL);
    last_px = px;
    last_us = now;
    return out;
}

// Frames per second x10 SINCE THE PREVIOUS CALL, so the caller's own cadence
// sets the averaging window. Returns 0 on the first call (no interval yet).
unsigned display_fps_x10(void)
{
    static uint32_t last_n;
    static int64_t  last_us;
    int64_t  now = esp_timer_get_time();
    uint32_t n   = s_frames;
    unsigned out = 0;
    if (last_us && now > last_us) {
        out = (unsigned)(((uint64_t)(n - last_n) * 10ULL * 1000000ULL) /
                         (uint64_t)(now - last_us));
    }
    last_n  = n;
    last_us = now;
    return out;
}

static lv_display_t *s_disp = NULL;
static bool s_flipped = false;   // false = normal landscape (90), true = upside-down (270)

bool display_lock(uint32_t timeout_ms)
{
    return bsp_display_lock(timeout_ms);
}

void display_unlock(void)
{
    bsp_display_unlock();
}

void display_set_brightness(int percent)
{
    bsp_display_brightness_set(percent);
}

// Call once, right after ui_init() returns (main.c) - see display_init()'s
// comment on why the backlight starts at 0 instead of coming on immediately.
//
// Deliberately a plain blocking ramp (vTaskDelay loop) on the CALLER's task,
// NOT an lv_anim_t. An lv_anim's steps are driven by the LVGL port's own
// timer/task, which is simultaneously doing its heaviest work of the whole
// session right here - the very first full-screen render (spectrum,
// waterfall, every button). An earlier lv_anim version of this fade was
// visibly coarse/jumpy and varied boot to boot (reported on real hardware,
// 2026-07-08) - the animation timer was being starved by that render, not
// running smoothly. Driving the brightness directly from app_main's own
// task (which has nothing else to do at this exact point) sidesteps LVGL's
// scheduler entirely, so the ramp is smooth and consistent regardless of
// how busy the first render is.
void display_fade_in_backlight(int target_percent)
{
    if (target_percent > 100) target_percent = 100;
    if (target_percent < 0) target_percent = 0;
    const int steps = 50;
    const int total_ms = 500;
    for (int i = 1; i <= steps; i++) {
        bsp_display_brightness_set((i * target_percent) / steps);
        vTaskDelay(pdMS_TO_TICKS(total_ms / steps));
    }
}

// Flip the landscape orientation 180 degrees (for upside-down mounting / which
// side the cables exit). Normal = LV_DISPLAY_ROTATION_90, flipped = _270. All
// widgets live in the same 1280x720 logical space, so LVGL re-maps the layout
// and ordinary touch automatically; only the raw-coord pinch handler in ui.c
// needs to know (via display_is_flipped). The lvgl_port lock is recursive, so
// this is safe both at boot (app_main) and from a drawer callback (LVGL task).
void display_set_flipped(bool flipped)
{
    if (!s_disp) return;
    s_flipped = flipped;
    if (display_lock(1000)) {
        lv_display_set_rotation(s_disp,
            flipped ? LV_DISPLAY_ROTATION_270 : LV_DISPLAY_ROTATION_90);
        lv_obj_invalidate(lv_screen_active());
        display_unlock();
    }
    ESP_LOGI(TAG, "Display rotation: %s", flipped ? "270 (flipped 180)" : "90 (normal)");
}

bool display_is_flipped(void)
{
    return s_flipped;
}

esp_err_t display_init(lv_display_t **out_disp)
{
    ESP_LOGI(TAG, "Bringing up display via local M5Stack BSP");

    // Cold-boot fix: PI4IO expander holds LCD_RST and TP_RST low until configured.
    // bsp_display_start_with_config does NOT do this for us; warm/soft resets only
    // work because the expander retains state from the previous boot. Without these
    // two calls, on a true power-on-from-off the panel never responds to DSI commands
    // and esp_lcd_new_panel_io_dbi hangs forever in the read-FIFO wait loop.
    ESP_ERROR_CHECK(bsp_i2c_init());
    bsp_io_expander_pi4ioe_init(bsp_i2c_get_handle());
    ESP_LOGI(TAG, "PI4IO expander initialized (LCD_RST + TP_RST released)");
    // Let panel and touch chip come out of reset before the BSP probes I2C
    // for the touch chip. Without this, the probe fires too fast on cold boot,
    // the touch chip does not respond, BSP picks ST7703/ILI9881C path, but
    // touch chip *does* respond by the time touch init runs -> driver mismatch
    // -> assert in bsp_display_indev_init_to_st7123.
    vTaskDelay(pdMS_TO_TICKS(120));

    /* -----------------------------------------------------------------
     * #285 EXPERIMENT: WHERE THE LVGL DRAW BUFFERS LIVE.
     *
     * This has been `buff_spiram = 1` since Phase 6.2, which puts the entire
     * hot path in PSRAM: the two canvases are blitted from PSRAM into a PSRAM
     * draw buffer, that draw buffer is then read STRIDED by LVGL's
     * rotate90_rgb565 (one 128-byte cache line fetched per 2-byte pixel), and
     * the result written to the PSRAM framebuffer. Three full-screen passes per
     * frame, all through the L2 cache, all on core 0 - where taskLVGL measures
     * 73.9%.
     *
     * Neutering the rotation recovered only ~7 of those points, which is the
     * clue: the strided read is expensive BECAUSE its source is PSRAM. From
     * internal SRAM there are no cache lines to fill, so both the blit
     * destination and the rotation source get dramatically cheaper.
     *
     * It costs internal RAM we did not have until now - and that is the join
     * between #284 and #285: halving the L2 cache returns 128 KB of exactly the
     * memory this needs. Two buffers at 16 lines is ~80 KB.
     *
     * ⚠ HYPOTHESIS, NOT A CONCLUSION. Four theories about core 0 were falsified
     * on hardware on 2026-08-28 alone, so this is selectable at RUNTIME and
     * defaults to the long-standing behaviour. `{"action":"gfx_exp",
     * "internal":true,"lines":16}` stores the choice and reboots, so an A/B
     * costs a reboot rather than a rebuild.
     *
     * ⛔ The size is checked against the LARGEST FREE BLOCK before asking, not
     * after failing: bsp_display_start_with_config() returning NULL aborts the
     * boot inside ESP_ERROR_CHECK, and retrying it after a partial init would
     * be worse than not trying. If the memory is not there we log and stay on
     * PSRAM.
     * ----------------------------------------------------------------- */
    bool want_internal = false;
    int  buf_lines     = 36;
    {
        nvs_handle_t h;
        if (nvs_open("devgfx", NVS_READONLY, &h) == ESP_OK) {
            uint8_t v = 0;
            if (nvs_get_u8(h, "internal", &v) == ESP_OK) want_internal = (v != 0);
            uint8_t l = 0;
            if (nvs_get_u8(h, "lines", &l) == ESP_OK && l >= 4 && l <= 64) buf_lines = l;
            nvs_close(h);
        }
        if (want_internal) {
            size_t need = (size_t)DISPLAY_H_RES * (size_t)buf_lines * 2;  // one buffer
            // Each buffer must be contiguous, and leave real headroom: the SD
            // mount alone needs ~26 KB of this pool later in the boot.
            size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_DMA);
            size_t total   = heap_caps_get_free_size(MALLOC_CAP_DMA);
            // ⛔ 200 KB, not 48 KB - and the first version of this guard was
            // WRONG in a way that took the device off the network. It checked
            // the pool as it stands at display_init(), which is early: ~174 KB
            // free. WiFi then takes ~111 KB of the SAME pool later in the boot,
            // and the SD mount another ~26 KB. With 2 x 20 KB of draw buffers
            // removed on top, the log read `int free=6KB (min=0KB lblk=2KB
            // LOW!)` and the web server died while LVGL carried on at 27.9 fps -
            // a device that looks fine on its screen and cannot be reached.
            // A guard on a pool that has not finished being spent has to budget
            // for the spending that is still coming.
            if (largest < need || total < need * 2 + 200 * 1024) {
                ESP_LOGW(TAG, "draw buffers stay in PSRAM: wanted 2 x %u B internal, "
                              "largest free block %u B, total %u B",
                         (unsigned)need, (unsigned)largest, (unsigned)total);
                want_internal = false;
            }
        }
        ESP_LOGW(TAG, "LVGL draw buffers: 2 x %d lines in %s",
                 buf_lines, want_internal ? "INTERNAL RAM" : "PSRAM");
    }

    bsp_display_cfg_t cfg = {
        .lvgl_port_cfg = {
            .task_priority    = 4,
            .task_stack       = 8192,
            // ⛔ CORE 0, and moving it is FALSIFIED ON HARDWARE (#284,
            // 2026-08-28). Everything LVGL does ends in the software 90-degree
            // rotation, which saturates this core (panadapter idle0 measured
            // 6.9-7.6% at a 256 KB L2 cache, 0.1-1.5% at 128 KB) while core 1
            // sat at 86-90% idle - so moving the task to core 1 looked free.
            //
            // It is not. fft_task also runs on core 1 at priority 4, the SAME
            // base priority as this task, and fft_task is the audio ring's ONLY
            // consumer for both the panadapter and FT8 capture. Within seconds
            // of booting with .task_affinity = 1 the log read
            //     audio: RX 47766 pairs/s ... DROPPED=48000 (ring full)
            // every second - USB delivering perfectly, 0 badpkt, and EVERY
            // sample discarded because nothing was draining the ring. The
            // spectrum died and it presented as "the Tab5 cannot see the radio".
            // That is #51 by another route.
            //
            // Any retry MUST also solve fft_task's scheduling (a higher
            // priority for it, or the rotation moved off the LVGL task entirely
            // rather than the task moved off the core). Do not just flip this.
            .task_affinity    = 0,
            .task_max_sleep_ms = 500,
            .timer_period_ms  = 5,
        },
        // 36 lines. ⛔ FALSIFIED ON HARDWARE 2026-08-28: cutting this to 12 to
        // shrink the rotation working set made core-0 idle WORSE, not better
        // (0.4-1.5% -> 0.0%, six samples, radio streaming). The theory was that
        // LVGL's rotate90_rgb565 re-traverses this whole buffer column by column
        // (1,280 times per flush, buff_spiram=1 so from PSRAM) and that 1280 x 36
        // x 2 = 92,160 B had stopped fitting in the 128 KB L2 cache. The strip's
        // residency is evidently NOT the binding cost - tripling the flush count
        // (20 -> 60 per frame) cost more than any cache benefit bought.
        // Recorded so nobody spends another power cycle on it.
        .buffer_size   = 0,   /* set from buf_lines just below */
        .double_buffer = true,
        .flags = {
            .buff_dma    = 0,   /* set from want_internal just below */
            .buff_spiram = 1,
            .sw_rotate   = 1,   // Phase 6.2: enable LVGL software rotation
        },
    };

    cfg.buffer_size        = DISPLAY_H_RES * buf_lines;
    cfg.flags.buff_dma     = want_internal ? 1 : 0;
    cfg.flags.buff_spiram  = want_internal ? 0 : 1;

    s_disp = bsp_display_start_with_config(&cfg);
    if (!s_disp) {
        ESP_LOGE(TAG, "bsp_display_start_with_config failed");
        return ESP_FAIL;
    }
    lv_display_add_event_cb(s_disp, disp_refr_ready_cb, LV_EVENT_REFR_READY, NULL);
    lv_display_add_event_cb(s_disp, disp_inval_area_cb, LV_EVENT_INVALIDATE_AREA, NULL);

    // Raise the AXI arbitration priority of the DW-GDMA masters (the DMA that
    // continuously streams the 1280x720 RGB565 framebuffer from PSRAM into the
    // MIPI-DSI bridge). Every QoS field resets to 0, so out of the box the
    // display fetch has NO precedence over CPU cache refills - and a sustained
    // CPU burst over PSRAM (FT4 decode + streaming STFT + a full decode-list
    // LVGL redraw, all PSRAM-resident) can crowd it out long enough to drop a
    // frame: the panel blanks to a light cyan/blue for one frame and recovers
    // ("full-screen cyan flash", seen mid-slot in FT4 every ~2-4 slots). The
    // DSI fetch is bounded (~110 MB/s of the >600 MB/s PSRAM budget), so
    // giving it near-top priority cannot starve the CPU - it just guarantees
    // the panel never misses a fetch window. 12 of 15: high, but below max in
    // case something ever genuinely needs the last word.
    axi_icm_ll_set_dw_gdma_qos_arbiter_prio(0, 12, 12);
    axi_icm_ll_set_dw_gdma_qos_arbiter_prio(1, 12, 12);
    ESP_LOGI(TAG, "DSI DW-GDMA AXI QoS priority raised (12/15, rd+wr, both masters)");

    // Start dark, not bsp_display_backlight_on() (=100% immediately): the
    // panel powers up with garbage/white framebuffer content that's visible
    // the instant the backlight is on, well before ui_init() has built
    // anything or LVGL has flushed a real frame - a jarring white flash.
    // display_fade_in_backlight() (called from main.c right after ui_init()
    // returns, i.e. once the first real frame is queued) ramps this up to
    // 100% over 250ms instead, so the app appears via a quick controlled
    // fade rather than an instant flash-then-swap.
    bsp_display_brightness_set(0);

    // Phase 6.2: rotate to landscape (panel is natively 720x1280 portrait)
    lv_display_set_rotation(s_disp, LV_DISPLAY_ROTATION_90);
    ESP_LOGI(TAG, "Phase 6.2: requested LV_DISPLAY_ROTATION_90 (landscape)");

    ESP_LOGI(TAG, "Display ready: native=%dx%d (rotated to landscape)", DISPLAY_H_RES, DISPLAY_V_RES);
    ESP_LOGI(TAG, "Free PSRAM=%zu KB, free internal=%zu KB",
             heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024,
             heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024);

    if (out_disp) *out_disp = s_disp;
    return ESP_OK;
}




