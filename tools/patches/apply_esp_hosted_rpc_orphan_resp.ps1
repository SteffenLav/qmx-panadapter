<#
.SYNOPSIS
    Stops esp_hosted's RPC receive thread deadlocking itself on a three-deep
    queue - the permanent "every Req[0x126] times out for ever while WiFi data
    keeps flowing" wedge this project has carried since 2026-06-30.

.DESCRIPTION
    host/drivers/rpc/core/rpc_core.c, in process_rpc_rx_msg(), queues a
    synchronous response FIRST and only then looks for the caller waiting on it:

        elem.buf = app_resp;
        elem.buf_len = sizeof(ctrl_cmd_t);

        if (g_h.funcs->_h_queue_item(rpc_rx_q, &elem, HOSTED_BLOCK_MAX)) {
            ESP_LOGE(TAG, "RPC Q put fail");
            goto free_buffers;
        }

        /* Call up rx ind to unblock caller */
        if (CALLBACK_AVAILABLE == is_sync_resp_sem_available(app_resp->uid))
            post_sync_resp_sem(app_resp);

    When the waiter has gone - its DEFAULT_RPC_RSP_TIMEOUT (5 s) expired, or
    set_sync_resp_sem() failed to create its semaphore and registered a NULL -
    the response is queued with nothing left to take it out. get_response() is
    the ONLY consumer of rpc_rx_q and it only runs when a semaphore is posted,
    which only this thread does. So the element is immortal.

    RPC_RX_QUEUE_SIZE is 3 (rpc_slave_if.h). The third orphan fills the queue,
    and the very next response has this same thread block in _h_queue_item()
    with portMAX_DELAY - for ever. It is both the only producer and the only
    thing that could wake the consumer, so nothing can ever free a slot. The
    RPC channel is dead until reboot; the data path is untouched, which is
    exactly why the symptom has always been "WiFi works, every RPC times out".

    Captured on the dev bench 2026-09-11, and the arithmetic is exact:

        201.7 s  sem create failed  x2   -> orphans 1 and 2
        632.2 s  sem create failed  x1   -> orphan 3, queue now full
        637.2 s  first Timeout waiting for Resp for Req[0x126]
        637 s -> 1431 s  every single 0x126 times out, ~1 per 5 s, 161 of them
        1103.2 s "task still writing Rx data to queue!" - the backlog behind
                 the blocked thread reaches the SDIO driver (1946 lines)
        1431.4 s SDIO RX buffer alloc failed, then the sdio_write_task assert

    The internal-RAM decline over that window (24 KB -> 5 KB, ~122 B per failed
    RPC) is NOT a per-failure leak, as an earlier note in this repo guessed: it
    is un-read RPC frames piling up behind the blocked thread. PSRAM fell
    1867 KB -> 278 KB the same way. Fixing the deadlock removes both.

    Two edits, both in rpc_core.c:

    1. set_sync_resp_sem() refuses to register a NULL semaphore. rpc_send_req()
       then frees app_req and returns FAILURE, RPC_SEND_REQ() returns NULL, and
       the request is never sent - so no response can arrive to be orphaned.
       This removes the seed. Registration happens BEFORE the request is queued
       for transmit, so there is no window where a legitimate response can
       arrive early and be mistaken for an orphan.

    2. process_rpc_rx_msg() asks for the waiter BEFORE queueing and drops a
       response nobody is waiting for, and bounds the enqueue wait at 1 s
       instead of portMAX_DELAY. Same precedent as the RX-oversize drain and
       the TX sendbuf patch: one frame is lost, the link survives.

    Edit 2 also closes a silent correctness bug. With even one orphan in the
    queue, get_response() dequeues the HEAD - the stale element - so every
    later synchronous RPC returned the PREVIOUS transaction's response, with no
    uid check and no complaint.

    A dropped response is freed through stock's own free_buffers: label, so a
    reply carrying nested allocations (a scan list, say) can leak those. That is
    stock's behaviour on its own error path, it is bounded by how rarely a drop
    happens, and the alternative is a wedged radio link.

    Edits managed_components/ (git-ignored): wiped by `idf.py fullclean`, a
    dependency refresh, or the release process's `rm -r managed_components/`.
    Re-run alongside the other esp_hosted scripts; tools/check_patches.py fails
    the build if it is missing. Idempotent.

.HOW TO USE
    powershell -ExecutionPolicy Bypass -File tools/patches/apply_esp_hosted_rpc_orphan_resp.ps1
#>

$ErrorActionPreference = "Stop"

$repo   = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$target = Join-Path $repo "managed_components/espressif__esp_hosted/host/drivers/rpc/core/rpc_core.c"

if (-not (Test-Path $target)) {
    Write-Host "rpc_core.c not found at:" -ForegroundColor Red
    Write-Host "  $target"
    Write-Host "(managed_components/ not fetched yet - run a build first, then re-run this.)"
    exit 1
}

$content = Get-Content -Raw -Path $target

if ($content -match "QMX_RPC_DROP_UNCLAIMED_RESP") {
    Write-Host "Already patched (RPC orphan-response deadlock) - nothing to do." -ForegroundColor Green
    exit 0
}

# --- edit 1: never register a NULL semaphore -------------------------------
$origSem = @'
		app_req->rx_sem = g_h.funcs->_h_create_semaphore(1);
		g_h.funcs->_h_get_semaphore(app_req->rx_sem, 0);
'@

$patchedSem = @'
		app_req->rx_sem = g_h.funcs->_h_create_semaphore(1);
		/* QMX_RPC_DROP_UNCLAIMED_RESP part 1 - see tools/patches/.
		 * Stock registered the uid with a NULL semaphore and sent the
		 * request anyway. The wait then failed instantly, the caller saw
		 * an error, and the response that DID arrive found no waiter -
		 * one immortal orphan in a queue only three deep. Refuse to send
		 * instead: rpc_send_req() frees app_req and returns FAILURE, and
		 * RPC_SEND_REQ() returns NULL to the caller. */
		if (!app_req->rx_sem) {
			ESP_LOGE(TAG, "no sem for req[0x%x] - not sending", app_req->msg_id);
			return CALLBACK_NOT_REGISTERED;
		}
		g_h.funcs->_h_get_semaphore(app_req->rx_sem, 0);
'@

# --- edit 2: ask for the waiter before queueing, and bound the put ---------
$origQ = @'
			if (g_h.funcs->_h_queue_item(rpc_rx_q, &elem, HOSTED_BLOCK_MAX)) {
				ESP_LOGE(TAG, "RPC Q put fail");
				goto free_buffers;
			}

			/* Call up rx ind to unblock caller */
			if (CALLBACK_AVAILABLE == is_sync_resp_sem_available(app_resp->uid))
				post_sync_resp_sem(app_resp);
'@

$patchedQ = @'
			/* QMX_RPC_DROP_UNCLAIMED_RESP part 2 - see tools/patches/.
			 * Stock queued first and looked for the waiter afterwards.
			 * A response whose waiter has gone - 5 s timeout expired, or
			 * no semaphore - was queued with nothing left to dequeue it,
			 * and rpc_rx_q holds THREE. The third orphan fills it and
			 * this thread then blocks here for portMAX_DELAY, for ever:
			 * it is both the only producer and the only thing that can
			 * wake the consumer. That is the permanent "every Req[0x126]
			 * times out while data keeps flowing" wedge. Ask first, drop
			 * what nobody awaits, and never wait unboundedly to enqueue.
			 * Dropping also stops the queue handing every later caller
			 * the PREVIOUS transaction's response off the head. */
			if (CALLBACK_AVAILABLE != is_sync_resp_sem_available(app_resp->uid)) {
				ESP_LOGW(TAG, "no waiter for resp[0x%x] uid %ld - dropping",
						app_resp->msg_id, (long)app_resp->uid);
				goto free_buffers;
			}

			if (g_h.funcs->_h_queue_item(rpc_rx_q, &elem, pdMS_TO_TICKS(1000))) {
				ESP_LOGE(TAG, "RPC Q put fail");
				goto free_buffers;
			}

			/* Call up rx ind to unblock caller */
			post_sync_resp_sem(app_resp);
'@

$contentLf = $content -replace "`r`n", "`n"

foreach ($pair in @(@($origSem, $patchedSem), @($origQ, $patchedQ))) {
    $o = $pair[0] -replace "`r`n", "`n"
    $p = $pair[1] -replace "`r`n", "`n"
    if (-not $contentLf.Contains($o)) {
        Write-Host "Could not find a stock block - rpc_core.c may have changed." -ForegroundColor Red
        Write-Host "Patch by hand and update this script. Missing block starts:" -ForegroundColor Red
        Write-Host ($o.Split("`n")[0])
        exit 1
    }
    $contentLf = $contentLf.Replace($o, $p)
}

[System.IO.File]::WriteAllText($target, $contentLf, (New-Object System.Text.UTF8Encoding $false))

Write-Host "Patched rpc_core.c: an unclaimed RPC response is dropped, not queued for ever." -ForegroundColor Green
Write-Host "  $target"
