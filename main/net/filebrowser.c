#include "filebrowser.h"
#include "webserver_ws.h"          // webserver_ws_set_paused
#include "dsp.h"                    // dsp_set_transfer_quiet
#include "storage/sd_archive.h"     // sd_archive_is_mounted / _lock / _unlock
#include "storage/sd_io_buf.h"      // sd_io_buf_alloc - DMA-aligned transfer buffer

#include "esp_log.h"
#include "cJSON.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>

static const char *TAG = "files";

#define FB_ROOT "/sdcard"

// Transfer buffer for download/upload. A whole number of 512 B sectors, so the
// length passed to FatFs is DMA-alignment-clean as well as the address.
#define FB_IO_BUF 4096

// ---------------------------------------------------------------------------
// The browser page. Self-contained (no external assets). All HTML attributes
// use single quotes and all dynamic HTML uses JS template literals (backticks)
// so this whole thing embeds as a C string with zero escaping.
// ---------------------------------------------------------------------------
static const char PAGE[] =
"<!doctype html><html><head><meta charset='utf-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>Tab5 microSD</title><style>"
"body{font-family:system-ui,Segoe UI,Roboto,sans-serif;margin:0;background:#111;color:#eee}"
"header{background:#1c1c1c;padding:14px 16px;font-size:19px;border-bottom:1px solid #333}"
"#bc{padding:10px 16px;color:#8ab;font-size:14px;word-break:break-all}"
"table{width:100%;border-collapse:collapse}"
"td,th{padding:11px 16px;border-bottom:1px solid #262626;text-align:left;font-size:15px}"
"a{color:#7bf;text-decoration:none}a:hover{text-decoration:underline}"
".dir{color:#fc8}.sz{color:#999;text-align:right;white-space:nowrap}"
".del{color:#f66;cursor:pointer}.del:hover{text-decoration:underline}"
"#upl{padding:14px 16px;border-top:1px solid #333;background:#1a1a1a;position:sticky;bottom:0}"
"button{background:#2a6;color:#fff;border:0;padding:9px 15px;border-radius:6px;cursor:pointer;font-size:15px}"
"#st{margin-left:10px;color:#9c9}"
"</style></head><body>"
"<header>Tab5 microSD</header><div id='bc'>loading...</div>"
"<table id='t'><tbody></tbody></table>"
"<div id='upl'><input type='file' id='f' multiple> <button onclick='up()'>Upload to this folder</button><span id='st'></span></div>"
"<script>"
"var cur='';"
"function fmt(n){if(n<1024)return n+' B';if(n<1048576)return (n/1024).toFixed(1)+' KB';return (n/1048576).toFixed(1)+' MB';}"
"function esc(s){return s.replace(/[&<>]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;'}[c]));}"
"function load(p){cur=p;fetch('/api/files?path='+encodeURIComponent(p)).then(r=>r.json()).then(d=>{"
"cur=d.path||'';"
"document.getElementById('bc').textContent='/sdcard'+(d.path||'')+(d.error?('  ['+d.error+']'):'');"
"var tb=document.querySelector('#t tbody');tb.innerHTML='';"
"if(d.path){var tr=document.createElement('tr');tr.innerHTML=`<td><a href='#' class='dir'>.. (up)</a></td><td></td><td></td>`;"
"tr.querySelector('a').onclick=()=>{var i=d.path.lastIndexOf('/');load(i>0?d.path.substring(0,i):'');return false;};tb.appendChild(tr);}"
"(d.entries||[]).sort((a,b)=>(b.dir-a.dir)||a.name.localeCompare(b.name)).forEach(e=>{"
"var np=(d.path||'')+'/'+e.name;var tr=document.createElement('tr');"
"if(e.dir){tr.innerHTML=`<td><a href='#' class='dir'>${esc(e.name)}/</a></td><td class='sz'></td><td></td>`;"
"tr.querySelector('a').onclick=()=>{load(np);return false;};}"
"else{tr.innerHTML=`<td><a href='/api/file?path=${encodeURIComponent(np)}' download>${esc(e.name)}</a></td><td class='sz'>${fmt(e.size)}</td><td><span class='del'>delete</span></td>`;"
"tr.querySelector('.del').onclick=()=>{if(confirm('Delete '+e.name+'?'))del(np);};}"
"tb.appendChild(tr);});"
"}).catch(e=>{document.getElementById('bc').textContent='error: '+e;});}"
"function del(p){fetch('/api/file?path='+encodeURIComponent(p),{method:'DELETE'}).then(()=>load(cur));}"
"function up(){var f=document.getElementById('f').files;if(!f.length){return;}var st=document.getElementById('st');var i=0;"
"(function nx(){if(i>=f.length){st.textContent='done';document.getElementById('f').value='';load(cur);return;}"
"var file=f[i];st.textContent='uploading '+file.name+' ...';"
"fetch('/api/file?path='+encodeURIComponent(cur+'/'+file.name),{method:'POST',body:file})"
".then(r=>{if(!r.ok)throw new Error(r.status);i++;nx();}).catch(e=>{st.textContent='failed: '+e;});})();}"
"load('');"
"</script></body></html>";

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static void url_decode(char *out, size_t outsz, const char *in)
{
    size_t o = 0;
    for (size_t i = 0; in[i] && o + 1 < outsz; i++) {
        if (in[i] == '%' && in[i + 1] && in[i + 2]) {
            char h[3] = { in[i + 1], in[i + 2], 0 };
            out[o++] = (char)strtol(h, NULL, 16);
            i += 2;
        } else {
            out[o++] = in[i];
        }
    }
    out[o] = 0;
}

// Resolve the ?path= query into a filesystem path (fs) and its normalized
// card-relative form (rel, "" for root). Returns false on a traversal attempt.
static bool fb_paths(httpd_req_t *req, char *fs, size_t fssz, char *rel, size_t relsz)
{
    char q[600], raw[512];
    raw[0] = 0;
    if (httpd_req_get_url_query_str(req, q, sizeof q) == ESP_OK) {
        if (httpd_query_key_value(q, "path", raw, sizeof raw) != ESP_OK) raw[0] = 0;
    }
    char dec[512];
    url_decode(dec, sizeof dec, raw);
    if (strstr(dec, "..")) return false;

    // Normalize to a leading '/' (or empty for root); strip trailing slashes.
    if (dec[0] && dec[0] != '/') {
        char tmp[513];
        snprintf(tmp, sizeof tmp, "/%s", dec);
        snprintf(rel, relsz, "%s", tmp);
    } else {
        snprintf(rel, relsz, "%s", dec);
    }
    size_t l = strlen(rel);
    while (l > 0 && rel[l - 1] == '/') rel[--l] = 0;

    snprintf(fs, fssz, "%s%s", FB_ROOT, rel);
    return true;
}

static void rm_rf(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0) return;
    if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(path);
        struct dirent *e;
        while (d && (e = readdir(d))) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            char c[880];
            snprintf(c, sizeof c, "%s/%s", path, e->d_name);
            rm_rf(c);
        }
        if (d) closedir(d);
        rmdir(path);
    } else {
        unlink(path);
    }
}

// ---------------------------------------------------------------------------
// Handlers
// ---------------------------------------------------------------------------

static esp_err_t page_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, PAGE, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t list_handler(httpd_req_t *req)
{
    char fs[600], rel[520];
    if (!fb_paths(req, fs, sizeof fs, rel, sizeof rel))
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad path");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "path", rel);
    cJSON *arr = cJSON_AddArrayToObject(root, "entries");

    if (!sd_archive_is_mounted()) {
        cJSON_AddStringToObject(root, "error", "no card mounted");
    } else if (!sd_archive_lock(3000)) {
        cJSON_AddStringToObject(root, "error", "card busy, retry");
    } else {
        DIR *d = opendir(fs);
        if (!d) {
            cJSON_AddStringToObject(root, "error", "not a folder");
        } else {
            struct dirent *e;
            while ((e = readdir(d))) {
                if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
                char c[880];
                snprintf(c, sizeof c, "%s/%s", fs, e->d_name);
                struct stat st;
                if (stat(c, &st) != 0) continue;
                cJSON *o = cJSON_CreateObject();
                cJSON_AddStringToObject(o, "name", e->d_name);
                cJSON_AddBoolToObject(o, "dir", S_ISDIR(st.st_mode));
                cJSON_AddNumberToObject(o, "size", (double)st.st_size);
                cJSON_AddItemToArray(arr, o);
            }
            closedir(d);
        }
        sd_archive_unlock();
    }

    char *s = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    esp_err_t err = httpd_resp_sendstr(req, s ? s : "{}");
    free(s);
    return err;
}

static esp_err_t download_handler(httpd_req_t *req)
{
    if (!sd_archive_is_mounted()) return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no card");

    char fs[600], rel[520];
    if (!fb_paths(req, fs, sizeof fs, rel, sizeof rel))
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad path");

    if (!sd_archive_lock(5000)) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "busy");

    struct stat st;
    if (stat(fs, &st) != 0 || S_ISDIR(st.st_mode)) {
        sd_archive_unlock();
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found");
    }

    // This transfer gets the WiFi TX path to itself (SD-during-WiFi discipline).
    webserver_ws_set_paused(true);
    dsp_set_transfer_quiet(true);

    const char *name = strrchr(rel, '/');
    name = name ? name + 1 : rel;
    char cd[560];
    snprintf(cd, sizeof cd, "attachment; filename=\"%s\"", name);
    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Content-Disposition", cd);

    FILE *f = fopen(fs, "r");
    esp_err_t err = ESP_OK;
    // Aligned so FatFs can DMA whole sectors straight into it. A stack buffer
    // is only 16-byte aligned, which sends every read down the SDMMC bounce
    // path and its per-call 512 B DMA allocation - the allocation that fails
    // on a low DMA pool and made this handler answer "not found" for a file
    // that was on the card. See main/storage/sd_io_buf.h.
    char *buf = f ? sd_io_buf_alloc(FB_IO_BUF) : NULL;
    if (f && !buf) {
        fclose(f);
        f = NULL;
        err = httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
    }
    if (!f) {
        if (err == ESP_OK) err = httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "open failed");
    } else {
        size_t n;
        while ((n = fread(buf, 1, FB_IO_BUF, f)) > 0 && err == ESP_OK)
            err = httpd_resp_send_chunk(req, buf, (ssize_t)n);
        /* ⛔ fread() RETURNS 0 FOR A READ ERROR AND FOR EOF ALIKE, so the loop
         * above cannot tell "file finished" from "the card failed". Without
         * this check err stays ESP_OK, the chunked terminator below is sent,
         * and the browser is handed a SHORT FILE THAT LOOKS WHOLE - the exact
         * hazard the note below describes for send failures, on the half
         * nobody checked.
         *
         * MEASURED 2026-10-04 on bench dev, one ordinary download, no stress:
         *   E sdmmc_cmd: sdmmc_read_sectors_dma: ... returned 0x101
         *   E heapwatch: ALLOC FAILED: 512 B caps=0x8 in task 'httpd'
         * and the client received 1336320 of 1725818 bytes with no error
         * anywhere - not in the log, not in the response. The SD read failed
         * because the bounce path could not get its 512 B from a DMA pool at
         * 2355 B; that is a separate problem, but it must not be able to
         * produce a silent truncation. The files reachable here include the
         * ADIF log and its backups. */
        if (err == ESP_OK && ferror(f)) {
            ESP_LOGE(TAG, "download %s: READ error after %ld bytes - aborting rather "
                          "than serving a truncated file", name, ftell(f));
            err = ESP_FAIL;
        }
        fclose(f);
        /* Only terminate the chunked body if it actually completed. The
         * terminator on a failed transfer hands the browser a short file that
         * looks whole - and the files reached through here include the ADIF log
         * and its backups, so that is a backup which restores fewer QSOs than
         * it should with nothing anywhere saying so. See the long note on
         * /api/adif in webserver.c. */
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "download %s: send failed mid-file (err 0x%x) - aborting the "
                          "connection rather than serving a truncated file", name, err);
        } else {
            httpd_resp_send_chunk(req, NULL, 0);
        }
    }

    sd_io_buf_free(buf);
    dsp_set_transfer_quiet(false);
    webserver_ws_set_paused(false);
    sd_archive_unlock();
    return err;
}

static esp_err_t upload_handler(httpd_req_t *req)
{
    if (!sd_archive_is_mounted()) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no card");

    char fs[600], rel[520];
    if (!fb_paths(req, fs, sizeof fs, rel, sizeof rel) || rel[0] == 0)
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad path");

    if (!sd_archive_lock(5000)) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "busy");

    webserver_ws_set_paused(true);
    dsp_set_transfer_quiet(true);

    FILE *f = fopen(fs, "w");
    esp_err_t err = ESP_OK;
    // One deferred failure reason instead of sending an error here. The old
    // shape sent 500 "create failed" from inside this block and then fell
    // through to the tail, which saw err == ESP_OK (httpd_resp_send_err
    // returns ESP_OK on a successful send) and appended {"ok":true} to the
    // same response. Found while aligning the buffer, fixed in the same pass.
    const char *fail = NULL;
    // Aligned for the same reason as the download path - see sd_io_buf.h.
    char *buf = f ? sd_io_buf_alloc(FB_IO_BUF) : NULL;
    if (!f) {
        fail = "create failed";
    } else if (!buf) {
        fclose(f);
        f = NULL;
        unlink(fs);
        fail = "out of memory";
    } else {
        int remaining = req->content_len;
        while (remaining > 0) {
            int r = httpd_req_recv(req, buf,
                                   FB_IO_BUF < (size_t)remaining ? FB_IO_BUF : (size_t)remaining);
            if (r <= 0) { err = ESP_FAIL; break; }
            if (fwrite(buf, 1, r, f) != (size_t)r) { err = ESP_FAIL; break; }
            remaining -= r;
        }
        fclose(f);
        if (err != ESP_OK) { unlink(fs); fail = "write failed"; }
    }

    sd_io_buf_free(buf);
    dsp_set_transfer_quiet(false);
    webserver_ws_set_paused(false);
    sd_archive_unlock();

    if (fail) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, fail);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t delete_handler(httpd_req_t *req)
{
    if (!sd_archive_is_mounted()) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no card");

    char fs[600], rel[520];
    if (!fb_paths(req, fs, sizeof fs, rel, sizeof rel) || rel[0] == 0)
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad path");

    if (!sd_archive_lock(5000)) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "busy");
    rm_rf(fs);
    sd_archive_unlock();

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

void filebrowser_register(httpd_handle_t server)
{
    static const httpd_uri_t u_page = { .uri = "/files",     .method = HTTP_GET,    .handler = page_handler };
    static const httpd_uri_t u_list = { .uri = "/api/files", .method = HTTP_GET,    .handler = list_handler };
    static const httpd_uri_t u_get  = { .uri = "/api/file",  .method = HTTP_GET,    .handler = download_handler };
    static const httpd_uri_t u_post = { .uri = "/api/file",  .method = HTTP_POST,   .handler = upload_handler };
    static const httpd_uri_t u_del  = { .uri = "/api/file",  .method = HTTP_DELETE, .handler = delete_handler };
    httpd_register_uri_handler(server, &u_page);
    httpd_register_uri_handler(server, &u_list);
    httpd_register_uri_handler(server, &u_get);
    httpd_register_uri_handler(server, &u_post);
    httpd_register_uri_handler(server, &u_del);
    ESP_LOGI(TAG, "file browser at /files -> %s", FB_ROOT);
}
