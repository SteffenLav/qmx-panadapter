<#
.SYNOPSIS
    Makes esp_hosted's SDIO RX dispatch task tolerate a failed payload-copy
    allocation instead of assert()-rebooting the device.

.DESCRIPTION
    host/drivers/transport/sdio/sdio_drv.c's sdio_process_rx_task() has:

        uint8_t * copy_payload = (uint8_t *)g_h.funcs->_h_malloc(buf_handle->payload_len);
        assert(copy_payload);

    Field-observed on bench1 (ESP32-P4, 2026-11-04, during a 14+ hour WSPR
    soak - serial-captured, panic_hook.c's crash record):

        assert failed: sdio_process_rx_task sdio_drv.c:1281 (copy_payload)
        task: sdio_process_rx core 0 after 51727.699 s of uptime

    Measured from the same capture: internal RAM had been sitting at a
    chronic ~1 KB minimum free for the entire session (not something that
    developed over time), and PSRAM free had separately stepped down by
    ~10 MB a few hours earlier. _h_malloc() for this one WiFi RX packet's
    payload copy finally landed on the wrong moment and returned NULL -
    stock code turned that into abort()+reboot, which on this board means
    the QMX also needs a manual power cycle (#74) - a whole unattended
    session's results (here: 250+ real WSPR decodes) lost to one failed
    allocation for one packet.

    Same family and same fix shape as apply_esp_hosted_sdio_rxbuf_tolerant.ps1
    (Uwe DL8UG): when there is nothing to copy this frame's payload into,
    free the original buffer handle and drop the frame (TCP/UDP retransmits;
    this project has no reliable-delivery expectation on the WiFi/BT control
    path either) instead of crashing the whole device. The two asserts
    immediately below (payload_len, payload) are left untouched - those guard
    a different, un-recovered condition, and apply_esp_hosted_assert_tolerant.ps1's
    own rule applies: remove an assert that duplicates a handled path, keep
    one where no handling exists.

    Because this edits managed_components/ (git-ignored), it is wiped by
    `idf.py fullclean`, a dependency refresh, or the release process's
    `rm -r managed_components/` - re-run alongside the other esp_hosted
    patches after any of those.

    Idempotent: running it twice is a no-op.

.HOW TO USE
    powershell -ExecutionPolicy Bypass -File tools/patches/apply_esp_hosted_sdio_copypayload_tolerant.ps1
#>

$ErrorActionPreference = "Stop"

$repo   = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$target = Join-Path $repo "managed_components/espressif__esp_hosted/host/drivers/transport/sdio/sdio_drv.c"

if (-not (Test-Path $target)) {
    Write-Host "sdio_drv.c not found at:" -ForegroundColor Red
    Write-Host "  $target"
    Write-Host "(managed_components/ not fetched yet - run a build first, then re-run this.)"
    exit 1
}

$content = Get-Content -Raw -Path $target

if ($content -match "QMX_PANADAPTER_SDIO_COPYPAYLOAD_TOLERANT") {
    Write-Host "Already patched (sdio_process_rx_task copy_payload tolerance) - nothing to do." -ForegroundColor Green
    exit 0
}

$original = @'
			if (chan_arr[buf_handle->if_type] && chan_arr[buf_handle->if_type]->rx) {
				/* TODO : Need to abstract heap_caps_malloc */
				uint8_t * copy_payload = (uint8_t *)g_h.funcs->_h_malloc(buf_handle->payload_len);
				assert(copy_payload);
				assert(buf_handle->payload_len);
				assert(buf_handle->payload);
				memcpy(copy_payload, buf_handle->payload, buf_handle->payload_len);
				H_FREE_PTR_WITH_FUNC(buf_handle->free_buf_handle, buf_handle->priv_buffer_handle);

				ret = chan_arr[buf_handle->if_type]->rx(chan_arr[buf_handle->if_type]->api_chan,
						copy_payload, copy_payload, buf_handle->payload_len);
				if (unlikely(ret))
					HOSTED_FREE(copy_payload);
			}
'@

$patched = @'
			if (chan_arr[buf_handle->if_type] && chan_arr[buf_handle->if_type]->rx) {
				/* TODO : Need to abstract heap_caps_malloc */
				uint8_t * copy_payload = (uint8_t *)g_h.funcs->_h_malloc(buf_handle->payload_len);
				if (!copy_payload) {
					/* QMX_PANADAPTER_SDIO_COPYPAYLOAD_TOLERANT: _h_malloc() can
					 * return NULL under the internal-RAM pressure this board
					 * runs under (see CLAUDE.md's internal RAM sections) -
					 * stock code asserted here, i.e. abort()+reboot. Field-
					 * observed on bench1 (ESP32-P4) after a 14+ hour WSPR
					 * soak: "assert failed: sdio_process_rx_task sdio_drv.c:
					 * 1281 (copy_payload)". There is nothing to copy this
					 * frame's payload into, so free the original buffer
					 * handle and drop the frame (one lost WiFi/BT packet,
					 * same retransmit assumption as the sibling RX-buffer
					 * patch) instead of rebooting the whole device.
					 */
					ESP_LOGW(TAG, "SDIO RX copy_payload alloc failed (len=%u) - dropping frame",
							(unsigned)buf_handle->payload_len);
					H_FREE_PTR_WITH_FUNC(buf_handle->free_buf_handle, buf_handle->priv_buffer_handle);
					continue;
				}
				assert(buf_handle->payload_len);
				assert(buf_handle->payload);
				memcpy(copy_payload, buf_handle->payload, buf_handle->payload_len);
				H_FREE_PTR_WITH_FUNC(buf_handle->free_buf_handle, buf_handle->priv_buffer_handle);

				ret = chan_arr[buf_handle->if_type]->rx(chan_arr[buf_handle->if_type]->api_chan,
						copy_payload, copy_payload, buf_handle->payload_len);
				if (unlikely(ret))
					HOSTED_FREE(copy_payload);
			}
'@

# Normalize line endings for the match (the tree uses LF)
$originalLf = $original -replace "`r`n", "`n"
$contentLf  = $content  -replace "`r`n", "`n"

if (-not $contentLf.Contains($originalLf)) {
    Write-Host "Could not find the stock copy_payload-assert block - sdio_drv.c may have changed." -ForegroundColor Red
    Write-Host "Patch by hand and update this script."
    exit 1
}

$patchedLf = $patched -replace "`r`n", "`n"
$result = $contentLf.Replace($originalLf, $patchedLf)
[System.IO.File]::WriteAllText($target, $result, (New-Object System.Text.UTF8Encoding $false))

Write-Host "Patched sdio_drv.c: sdio_process_rx_task now tolerates a failed payload-copy allocation." -ForegroundColor Green
Write-Host "  $target"
