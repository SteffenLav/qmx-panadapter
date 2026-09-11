# Contributed by Uwe DL8UG, who found the problem this patch fixes while
# building the spot map and wrote the fix.
<#
.SYNOPSIS
    Makes esp_hosted's SDIO RX task tolerate a failed buffer allocation
    instead of assert()-rebooting the device.

.DESCRIPTION
    host/drivers/transport/sdio/sdio_drv.c's sdio_read_task() has:

        rxbuff = sdio_rx_get_buffer(len_from_slave);
        assert(rxbuff);

    Field-observed on the Tab5 (ESP32-P4, 2026-09-11, serial-captured via
    panic_hook.c's crash record - see main/util/panic_hook.c):

        assert failed: sdio_read_task sdio_drv.c:999 (rxbuff)
        task: sdio_read core 0 after 5836.513 s of uptime

    sdio_rx_get_buffer() draws from the fixed RX_NONE mempool (or, in other
    RX modes, the internal DMA-capable heap) - both scarce, contended
    resources on this board (see CLAUDE.md's MALLOC_CAP_DMA sections). Under
    sustained load (WiFi + net/pskr_self.c's live MQTT client + the usual
    background feeds) that pool can run dry, and stock code turns that into
    a hard abort() - i.e. a full device reboot, at ~97 minutes of uptime in
    the observed case.

    The fix mirrors the file's OWN existing precedent immediately above this
    site (the oversized-pending-length recovery, already patched by
    apply_esp_hosted_sdio_recovery.ps1): when there is no buffer to receive
    a frame into, drain and discard the pending bytes from the slave in one
    CMD53 read (reusing the same sdio_oversize_drain_buf scratch buffer) and
    advance sdio_rx_byte_count so the link stays in sync. One frame is lost
    (TCP/UDP retransmits, or in this project's case an MQTT PING/frame that
    simply arrives again) - the link survives instead of the whole device
    rebooting.

    Because this edits managed_components/ (git-ignored), it is wiped by
    `idf.py fullclean`, a dependency refresh, or the release process's
    `rm -r managed_components/` - re-run alongside
    apply_esp_hosted_psram.ps1 and apply_esp_hosted_sdio_recovery.ps1 after
    any of those.

    Idempotent: running it twice is a no-op.

.HOW TO USE
    powershell -ExecutionPolicy Bypass -File tools/patches/apply_esp_hosted_sdio_rxbuf_tolerant.ps1
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

if ($content -match "QMX_PANADAPTER_SDIO_RXBUF_TOLERANT") {
    Write-Host "Already patched (sdio_read_task rxbuf tolerance) - nothing to do." -ForegroundColor Green
    exit 0
}

$original = @'
		/* Allocate rx buffer */
		rxbuff = sdio_rx_get_buffer(len_from_slave);
		assert(rxbuff);

		data_left = len_from_slave;
		pos = rxbuff;
'@

$patched = @'
		/* Allocate rx buffer */
		rxbuff = sdio_rx_get_buffer(len_from_slave);
		if (!rxbuff) {
			/* QMX_PANADAPTER_SDIO_RXBUF_TOLERANT: sdio_rx_get_buffer() can
			 * return NULL when the RX_NONE mempool (or, in other RX modes,
			 * the internal DMA-capable heap) is exhausted under sustained
			 * load - stock code asserted here, i.e. abort()+reboot. Field-
			 * observed on the Tab5 (ESP32-P4) after ~97 minutes of uptime
			 * with WiFi + MQTT (net/pskr_self.c) + several background feeds
			 * running: "assert failed: sdio_read_task sdio_drv.c:999
			 * (rxbuff)". There is nothing to receive this frame into, so -
			 * identical in spirit to the oversize-delta recovery above,
			 * reusing the same scratch buffer - drain and discard the
			 * pending bytes (one lost frame, TCP/UDP retransmits) and keep
			 * the link alive instead of rebooting the whole device.
			 */
			ESP_LOGW(TAG, "SDIO RX buffer alloc failed (len=%lu) - draining to recover",
					(unsigned long)len_from_slave);
			if (!sdio_oversize_drain_buf)
				sdio_oversize_drain_buf = (uint8_t *)MEM_ALLOC(MAX_SDIO_BUFFER_SIZE);
			if (sdio_oversize_drain_buf) {
				uint32_t block_len = ((len_from_slave + ESP_BLOCK_SIZE - 1) / ESP_BLOCK_SIZE) * ESP_BLOCK_SIZE;
				if (g_h.funcs->_h_sdio_read_block(sdio_handle,
						ESP_SLAVE_CMD53_END_ADDR - len_from_slave,
						sdio_oversize_drain_buf, block_len, ACQUIRE_LOCK)) {
					ESP_LOGE(TAG, "SDIO rxbuf-alloc-fail drain read failed");
				}
			}
			sdio_rx_byte_count = (sdio_rx_byte_count + len_from_slave) % ESP_RX_BYTE_MAX;
			SDIO_DRV_UNLOCK();
			continue;
		}

		data_left = len_from_slave;
		pos = rxbuff;
'@

# Normalize line endings for the match (the tree uses LF)
$originalLf = $original -replace "`r`n", "`n"
$contentLf  = $content  -replace "`r`n", "`n"

if (-not $contentLf.Contains($originalLf)) {
    Write-Host "Could not find the stock rxbuff-assert block - sdio_drv.c may have changed." -ForegroundColor Red
    Write-Host "Patch by hand and update this script."
    exit 1
}

$patchedLf = $patched -replace "`r`n", "`n"
$result = $contentLf.Replace($originalLf, $patchedLf)
[System.IO.File]::WriteAllText($target, $result, (New-Object System.Text.UTF8Encoding $false))

Write-Host "Patched sdio_drv.c: sdio_read_task now tolerates a failed RX buffer allocation." -ForegroundColor Green
Write-Host "  $target"
