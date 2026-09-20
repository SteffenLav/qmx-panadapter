#!/usr/bin/env python3
"""Fail the build when a standing patch is missing.

WHY THIS EXISTS (TODO #181, written 2026-08-17 the same evening it was needed):

Three patches had silently vanished from `managed_components/` - a git-ignored
directory, so nothing in the repo could show it - and the resulting binary wedged
WiFi within about four minutes of every boot. The symptom is indistinguishable
from a hardware fault: the device stays completely healthy (no reboot, no panic,
audio still streaming) while the C6 co-processor stops answering. It cost an
evening and TWO wrong hypotheses (BLE scanning, then DMA starvation) before anyone
thought to check whether the patches were still applied.

The only thing that had ever said "re-apply them" was the release process. A plain
`idf.py build` would happily produce that broken binary, which is the gap this
closes.

TWO RULES FOR THIS FILE:

  1. The markers below are the SAME strings each apply_*.ps1 script tests for its
     own idempotency. Do not invent a different marker here - if the two ever
     disagree, this check starts lying, and a check that lies about patches is
     worse than no check at all. When a patch script's marker changes, change it
     here in the same commit.

     For the two USB patches that cannot log (#7 and #8) the marker is the
     COUNTER SYMBOL, not a date comment. Those patches turn an abort() into a
     survivable error silently, so a tolerant-but-silent version of them is
     indistinguishable from no patch at all in a diag log - it must not pass this
     check. Testing for the symbol makes "patched" mean "patched AND observable"
     (TODO #189).

  2. Missing patches are an ERROR, never a warning. A warning scrolls past in a
     long IDF build, which is exactly how it would go unnoticed again.

A missing FILE is only a warning: the managed_components tree may legitimately not
be fetched yet on the very first configure, and the IDF-tree patches live outside
this repo entirely (they are per-build-machine, wiped by an IDF reinstall). It is a
present-but-unpatched file that means trouble.
"""

import os
import sys

# (script to run, path relative to what, path, marker, why it matters)
#
# "repo" = this checkout; "idf" = the pinned ESP-IDF install tree.
PATCHES = [
    ("apply_esp_hosted_psram.ps1", "repo",
     "managed_components/espressif__esp_hosted/host/port/include/os_wrapper.h",
     "extra_heap_caps = MALLOC_CAP_SPIRAM",
     "WiFi transport buffers stay in scarce internal DRAM; the device reboots "
     "under QMX+FT8 load when WiFi TX bursts"),

    ("apply_lvgl_port_tick_no_mutex.ps1", "repo",
     "managed_components/espressif__esp_lvgl_port/src/lvgl9/esp_lvgl_port.c",
     "PATCHED (qmx-panadapter, 2026-09-13) tick without mutex",
     "the 2 ms LVGL tick takes a mutex on the esp_timer task (priority 22); "
     "when it collides with a touch read taskLVGL inherits 22 for a whole "
     "redraw and starves USB-CDC, audio and the UAC driver - CAT timeouts and "
     "lost audio whenever the manual or an overlay opens"),

    ("apply_lv_event_chain_guard.ps1", "repo",
     "managed_components/lvgl__lvgl/src/misc/lv_event.c",
     "qmx_lv_event_chain_bad",
     "LVGL walks its in-flight event chain on EVERY object destruction; a "
     "corrupt link takes a Load access fault and reboots the device, and the "
     "marker is the COUNTER symbol because a silent tolerant patch is "
     "indistinguishable from a missing one (#189/#329)"),

    ("apply_esp_hosted_sdio_recovery.ps1", "repo",
     "managed_components/espressif__esp_hosted/host/drivers/transport/sdio/sdio_drv.c",
     "SDIO RX oversize",
     "an oversized SDIO pending-byte delta livelocks the link: WiFi dies within "
     "minutes and every RPC times out forever (reboot is the only way out)"),

    # Same FILE as the row above, different marker. sdio_drv.c now carries
    # several of our fixes and one apply script restores all of them, so a row
    # per marker is the only way a partially-restored copy shows up as missing.
    ("apply_esp_hosted_sdio_recovery.ps1", "repo",
     "managed_components/espressif__esp_hosted/host/drivers/transport/sdio/sdio_drv.c",
     "QMX_SDIO_INIT_FAIL_TOLERANT",
     "a failed SDIO card init RETURNS from a FreeRTOS task function, executing "
     "a ret to address 0 - the device reboots (MEPC=0, RA=0) instead of simply "
     "losing WiFi, and that warm reset also wedges the attached QMX (#74)"),

    ("apply_esp_hosted_sdio_rxbuf_tolerant.ps1", "repo",
     "managed_components/espressif__esp_hosted/host/drivers/transport/sdio/sdio_drv.c",
     "QMX_PANADAPTER_SDIO_RXBUF_TOLERANT",
     "sdio_read_task() abort()s the device when its RX buffer pool is "
     "momentarily exhausted under load - observed at ~97 minutes of uptime "
     "with WiFi + MQTT self-spotting running"),

    ("apply_esp_hosted_sdio_write_avail_tolerant.ps1", "repo",
     "managed_components/espressif__esp_hosted/host/drivers/transport/sdio/sdio_drv.c",
     "QMX_SDIO_WRITE_AVAIL_TOLERANT",
     "sdio_is_write_buffer_available() gave the slave two back-to-back chances "
     "and then REBOOTED the device - a deliberate restart, so no crash record, "
     "and a warm reset with the radio attached wedges the QMX (#74)"),

    ("apply_esp_hosted_rpc_orphan_resp.ps1", "repo",
     "managed_components/espressif__esp_hosted/host/drivers/rpc/core/rpc_core.c",
     "QMX_RPC_DROP_UNCLAIMED_RESP",
     "an RPC response whose waiter has timed out is queued with nothing left "
     "to dequeue it; rpc_rx_q holds THREE, and the third orphan makes the RX "
     "thread block on itself for ever - the permanent 0x126-times-out wedge"),

    # Layered on the assert-tolerant patch below, which replaces os_wrapper.c
    # wholesale - the script refuses to run before it.
    ("apply_esp_hosted_task_stacks.ps1", "repo",
     "managed_components/espressif__esp_hosted/host/port/src/os_wrapper.c",
     "QMX_HOSTED_TASK_STACKS",
     "esp_hosted hardcodes six 5 KB task stacks in internal DMA-capable RAM; "
     "sized to measured use and the RPC pair moved to PSRAM, ~15 KB back to "
     "the pool whose exhaustion crashed the device 2026-09-11"),

    ("apply_esp_hosted_sdio_sendbuf_tolerant.ps1", "repo",
     "managed_components/espressif__esp_hosted/host/drivers/transport/sdio/sdio_drv.c",
     "QMX_SDIO_SENDBUF_TOLERANT",
     "sdio_write_task() abort()s the device when a send buffer cannot be "
     "allocated, with the drop path one line below - captured 2026-09-11 at "
     "24 min, after a boot that started with the DMA pool at 99 bytes"),

    ("apply_esp_hosted_assert_tolerant.ps1", "repo",
     "managed_components/espressif__esp_hosted/host/port/src/os_wrapper.c",
     "QMX_PANADAPTER_ASSERT_TOLERANT_PATCH_MARKER",
     "esp_hosted abort()s the device when handed a NULL semaphore - the correct "
     "return was already beside the assert. Field-captured 2026-08-25 from the "
     "1 Hz WiFi poll, and the warm reset wedged the QMX with it (#74)"),

    ("apply_cdc_acm_close_tolerant.ps1", "repo",
     "managed_components/espressif__usb_host_cdc_acm/cdc_acm_host.c",
     "PATCHED (qmx-panadapter, 2026-08-16)",
     "closing a CDC interface while a URB is in flight abort()s the device, i.e. "
     "a reboot on a busy port"),

    ("apply_hcd_bulk_error_recovery.ps1", "idf",
     "components/usb/hcd_dwc.c",
     "PATCHED (qmx-panadapter, 2026-07-16)",
     "a transient USB bulk error abort()s the device instead of being retried"),

    # NOTE hcd_dwc.c carries THREE of our patches - #4 above, #8 and #9 here -
    # with separate markers. Applying one does not apply the others, so each
    # needs its own row: do not collapse them into one check on the filename.
    ("apply_hcd_buffer_parse_error_tolerant.ps1", "idf",
     "components/usb/hcd_dwc.c",
     "g_qmx_usb_pipe_event_unexpected",
     "a failed URB carrying a pipe event the error parser thinks impossible "
     "abort()s the device - observed at 7h into a healthy session, and that warm "
     "reset then left the QMX unable to re-enumerate for the rest of the night"),

    ("apply_hcd_buffer_parse_no_urb_tolerant.ps1", "idf",
     "components/usb/hcd_dwc.c",
     "g_qmx_usb_buffer_parse_no_urb",
     "a DMA buffer reaching the parser with no URB attached assert()s and reboots "
     "the device - observed when the operator restarted the QMX, and that warm "
     "reset takes the radio down with it (#74)"),

    ("apply_hub_recover_tolerant.ps1", "idf",
     "components/usb/hub.c",
     "PATCHED (qmx-panadapter, 2026-08-03)",
     "a root-port recover that races the hub FSM abort()s the device"),

    ("apply_usb_dwc_hal_chan_error_tolerant.ps1", "idf",
     "components/hal/usb_dwc_hal.c",
     "g_qmx_usb_chan_err_no_halt",
     "a USB channel error arriving without the halt bit abort()s the device - and "
     "that warm reset then leaves the QMX unable to re-enumerate for hours"),

    ("apply_httpd_ws_dead_socket_close.ps1", "idf",
     "components/esp_http_server/src/httpd_ws.c",
     "QMX PATCH #9: dead WS socket must close, not spin",
     "a websocket whose socket dies is marked closed but never closed, so httpd "
     "spins on recv at ~158/s and starves fft_task - the audio ring overflows and "
     "FT8 decodes nothing until something else frees the socket"),

    ("apply_fatfs_exfat.ps1", "idf",
     "components/fatfs/src/ffconf.h",
     "#define FF_FS_EXFAT\t1",
     "microSD cards larger than 32 GB (exFAT) will not mount"),

    ("apply_mqtt_client_psram_task.ps1", "idf",
     "components/mqtt/esp-mqtt/mqtt_client.c",
     "PATCHED (qmx-panadapter, 2026-09-10)",
     "esp_mqtt_client_start() always allocated its task stack from internal "
     "RAM with no PSRAM option; on this board that RAM is chronically tight "
     "after WiFi bring-up and the allocation failed outright, silently "
     "killing live PSK Reporter self-spotting (net/pskr_self.c) for the "
     "whole session"),

    # Same FILE as the row above, different marker - mqtt_client.c now carries
    # two of our fixes and one apply script does not apply the other.
    ("apply_mqtt_event_loop_create_checked.ps1", "idf",
     "components/mqtt/esp-mqtt/mqtt_client.c",
     "QMX_MQTT_EVENT_LOOP_CREATE_CHECKED",
     "esp_mqtt_client_init() never checked esp_event_loop_create()'s return "
     "value; under the same boot-time memory pressure it returns a non-NULL "
     "client with event_loop_handle left NULL, and the first "
     "esp_mqtt_client_register_event() call asserts - crash-reboot-crash loop, "
     "captured 2026-09-20 task pskr_self at 22.6 s of uptime"),
]


def marker_present(path, marker):
    """Substring test, whitespace-insensitive so a tab/space difference in a
    #define cannot produce a false alarm."""
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as fh:
            text = fh.read()
    except OSError:
        return None                      # unreadable == treat as absent-file
    squash = " ".join(text.split())
    return " ".join(marker.split()) in squash


def main():
    repo = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    idf = os.environ.get("IDF_PATH", "")

    missing, absent = [], []
    for script, root, rel, marker, why in PATCHES:
        base = repo if root == "repo" else idf
        if not base:
            absent.append((script, rel, "IDF_PATH is not set"))
            continue
        path = os.path.join(base, rel.replace("/", os.sep))
        if not os.path.isfile(path):
            absent.append((script, rel, "file not present"))
            continue
        if not marker_present(path, marker):
            missing.append((script, rel, why))

    for script, rel, note in absent:
        print("check_patches: SKIP %s (%s) - %s" % (rel, script, note))

    if missing:
        print("")
        print("=" * 78)
        print("check_patches: %d STANDING PATCH(ES) MISSING - refusing to build" % len(missing))
        print("=" * 78)
        for script, rel, why in missing:
            print("")
            print("  %s" % rel)
            print("     consequence : %s" % why)
            print("     fix         : powershell -File tools/patches/%s" % script)
        print("")
        print("  These live outside version control (managed_components/ is git-ignored;")
        print("  the IDF-tree ones are per-build-machine), so a clean fetch, an")
        print("  `idf.py fullclean`, a dependency refresh or an IDF reinstall silently")
        print("  removes them. The resulting binary looks fine and then fails on")
        print("  hardware in a way that reads as a hardware fault - see TODO #180.")
        print("")
        return 1

    print("check_patches: all %d standing patches present" % len(PATCHES))
    return 0


if __name__ == "__main__":
    sys.exit(main())
