#include "rigctld_server.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"

#include "cat.h"
#include "ui.h"

static const char *TAG = "rigctld";

#define RIGCTLD_PORT          4532
#define RIGCTLD_MAX_CLIENTS   4
#define RIGCTLD_LINE_BUF      256
#define RIGCTLD_RESP_BUF      4096

static int  s_listen_fd = -1;
static TaskHandle_t s_listen_task = NULL;
static volatile int s_client_count = 0;

/* Hamlib RPRT codes used in our replies. */
#define RPRT_OK              0
#define RPRT_E_PROTO       (-8)    /* invalid parameter */
#define RPRT_E_RIG         (-11)   /* function not implemented */

/* ---- Helpers ---------------------------------------------------------- */

static int sock_send_all(int fd, const char *buf, size_t len)
{
    size_t total = 0;
    while (total < len) {
        int n = send(fd, buf + total, len - total, 0);
        if (n <= 0) return -1;
        total += (size_t)n;
    }
    return 0;
}

static int sock_recv_line(int fd, char *buf, size_t cap)
{
    /* Read until \n or full. Returns line length excluding \n, or -1 on close/err. */
    size_t i = 0;
    while (i < cap - 1) {
        char c;
        int n = recv(fd, &c, 1, 0);
        if (n <= 0) return -1;
        if (c == '\r') continue;
        if (c == '\n') {
            buf[i] = 0;
            return (int)i;
        }
        buf[i++] = c;
    }
    buf[i] = 0;
    return (int)i;
}

/* Translate Kenwood mode digit (cached in ui.c) back to Hamlib mode name. */
static const char *kenwood_to_hamlib(const char *mode_str)
{
    if (!mode_str) return "USB";
    if (strcmp(mode_str, "LSB") == 0)    return "LSB";
    if (strcmp(mode_str, "USB") == 0)    return "USB";
    if (strcmp(mode_str, "CW")  == 0)    return "CW";
    if (strcmp(mode_str, "FM")  == 0)    return "FM";
    if (strcmp(mode_str, "AM")  == 0)    return "AM";
    if (strcmp(mode_str, "DiGi") == 0)   return "PKTUSB";
    if (strcmp(mode_str, "CW-R") == 0)   return "CWR";
    if (strcmp(mode_str, "DiGi-R") == 0) return "PKTLSB";
    return "USB";
}

/* Map ESP-IDF error to Hamlib RPRT. */
static int esp_to_rprt(esp_err_t err)
{
    switch (err) {
        case ESP_OK:                return RPRT_OK;
        case ESP_ERR_INVALID_ARG:   return RPRT_E_PROTO;
        case ESP_ERR_INVALID_STATE: return RPRT_E_RIG;
        case ESP_ERR_TIMEOUT:       return RPRT_E_RIG;
        default:                    return RPRT_E_RIG;
    }
}

/* ---- Dispatch --------------------------------------------------------- */

/* The big dump_state response is what Hamlib sends back to negotiate rig
   capabilities. Minimum viable set; fields are well-documented in Hamlib
   source (src/rigctl_parse.c). We claim to be Kenwood-ish (rig id 2). */
static const char DUMP_STATE[] =
    "0\n"                                 /* protocol version */
    "2\n"                                 /* rig model: NET rigctl */
    "2\n"                                 /* ITU region */
    /* RX freq ranges: start end modes low_pwr high_pwr vfo ant */
    "1800000.000000 30000000.000000 0x2ef -1 -1 0x1 0x0\n"
    "0 0 0 0 0 0 0\n"                     /* end of RX ranges */
    /* TX freq ranges (we report none -- we are RX-only at the panadapter level) */
    "0 0 0 0 0 0 0\n"                     /* end of TX ranges */
    /* tuning_steps: mode_mask step */
    "0x2ef 1\n"
    "0 0\n"                               /* end */
    /* filters: mode_mask width */
    "0x82 500\n"                          /* CW    500 Hz */
    "0x1 2400\n"                          /* LSB   2.4 kHz */
    "0x2 2400\n"                          /* USB   2.4 kHz */
    "0x60 3200\n"                         /* DIGI  3.2 kHz */
    "0 0\n"                               /* end */
    "0\n"                                 /* max_rit */
    "0\n"                                 /* max_xit */
    "0\n"                                 /* max_ifshift */
    "0\n"                                 /* announces */
    "\n"                                  /* preamp list */
    "\n"                                  /* attenuator list */
    "0\n"                                 /* has_get_func */
    "0\n"                                 /* has_set_func */
    "0\n"                                 /* has_get_level */
    "0\n"                                 /* has_set_level */
    "0\n"                                 /* has_get_parm */
    "0\n"                                 /* has_set_parm */
    "vfo_ops=0x0\n"
    "ptt_type=0x0\n"
    "targetable_vfo=0x0\n"
    "done\n";

static void handle_line(int fd, char *line)
{
    char resp[RIGCTLD_RESP_BUF];
    int  rlen = 0;

    /* Skip leading whitespace */
    while (*line == ' ' || *line == '\t') line++;
    if (*line == 0) return;

    ESP_LOGD(TAG, "fd=%d cmd: '%s'", fd, line);

    /* dump_state -- backslash command */
    if (strncmp(line, "\\dump_state", 11) == 0) {
        rlen = snprintf(resp, sizeof(resp), "%s", DUMP_STATE);
        sock_send_all(fd, resp, (size_t)rlen);
        return;
    }
    if (strncmp(line, "\\chk_vfo", 8) == 0) {
        rlen = snprintf(resp, sizeof(resp), "CHKVFO 0\n");
        sock_send_all(fd, resp, (size_t)rlen);
        return;
    }
    if (strncmp(line, "\\get_powerstat", 14) == 0) {
        /* 1 = RIG_POWER_ON */
        rlen = snprintf(resp, sizeof(resp), "1\nRPRT 0\n");
        sock_send_all(fd, resp, (size_t)rlen);
        return;
    }
    if (strncmp(line, "\\get_lock_mode", 14) == 0) {
        /* 0 = not locked */
        rlen = snprintf(resp, sizeof(resp), "0\nRPRT 0\n");
        sock_send_all(fd, resp, (size_t)rlen);
        return;
    }
    if (strncmp(line, "\\get_vfo", 9) == 0) {
        rlen = snprintf(resp, sizeof(resp), "VFOA\nRPRT 0\n");
        sock_send_all(fd, resp, (size_t)rlen);
        return;
    }

    /* Single-letter commands: first non-space token */
    char cmd = line[0];
    char *args = line + 1;
    while (*args == ' ' || *args == '\t') args++;

    switch (cmd) {
        case 'f': {  /* get_freq */
            rlen = snprintf(resp, sizeof(resp), "%lu\n",
                            (unsigned long)cat_get_frequency());
            break;
        }
        case 'F': {  /* set_freq */
            char *end;
            unsigned long hz = strtoul(args, &end, 10);
            esp_err_t err = (hz > 0 && end != args)
                            ? cat_set_frequency((uint32_t)hz)
                            : ESP_ERR_INVALID_ARG;
            rlen = snprintf(resp, sizeof(resp), "RPRT %d\n", esp_to_rprt(err));
            break;
        }
        case 'm': {  /* get_mode -> "MODE\nPASSBAND" */
            const char *hl = kenwood_to_hamlib(ui_get_mode_str());
            uint32_t pb = ui_get_passband_width_hz();
            if (pb == 0) {
                /* Mode default if FW not yet reported */
                if (strcmp(hl, "CW")  == 0) pb = 500;
                else if (strcmp(hl, "PKTUSB") == 0 || strcmp(hl, "PKTLSB") == 0) pb = 3200;
                else pb = 2400;
            }
            rlen = snprintf(resp, sizeof(resp), "%s\n%lu\n", hl, (unsigned long)pb);
            break;
        }
        case 'M': {  /* set_mode MODE PASSBAND */
            char mode[16] = {0};
            unsigned long pb = 0;
            int n = sscanf(args, "%15s %lu", mode, &pb);
            esp_err_t err = ESP_ERR_INVALID_ARG;
            if (n >= 1) {
                err = cat_set_mode(mode);
                if (err == ESP_OK && n == 2 && pb > 0) {
                    /* Best-effort passband set; ignore failure */
                    (void)cat_set_passband_hz((uint32_t)pb);
                }
            }
            rlen = snprintf(resp, sizeof(resp), "RPRT %d\n", esp_to_rprt(err));
            break;
        }
        case 'v':    /* get_vfo */
            rlen = snprintf(resp, sizeof(resp), "VFOA\n");
            break;
        case 's':    /* get_split_vfo */
            rlen = snprintf(resp, sizeof(resp), "0\nVFOA\n");
            break;
        case 't':    /* get_ptt */
            rlen = snprintf(resp, sizeof(resp), "0\n");
            break;
        case 'q':    /* quit */
            /* Caller breaks the loop on -1 from recv after close */
            shutdown(fd, SHUT_RDWR);
            return;
        default:
            /* Backslash extended commands we do not implement get RPRT -11
               (function not implemented); everything else gets -8 (proto). */
            if (cmd == '\\') {
                rlen = snprintf(resp, sizeof(resp), "RPRT %d\n", RPRT_E_RIG);
            } else {
                rlen = snprintf(resp, sizeof(resp), "RPRT %d\n", RPRT_E_PROTO);
            }
            ESP_LOGW(TAG, "fd=%d unknown cmd '%s'", fd, line);
            break;
    }
    if (rlen > 0) sock_send_all(fd, resp, (size_t)rlen);
}

/* ---- Per-client task -------------------------------------------------- */

static void client_task(void *arg)
{
    int fd = (int)(intptr_t)arg;
    ESP_LOGI(TAG, "fd=%d client connected (count=%d)", fd, s_client_count);

    char line[RIGCTLD_LINE_BUF];
    for (;;) {
        int n = sock_recv_line(fd, line, sizeof(line));
        if (n < 0) break;
        if (n == 0) continue;
        handle_line(fd, line);
    }

    close(fd);
    s_client_count--;
    ESP_LOGI(TAG, "fd=%d client disconnected (count=%d)", fd, s_client_count);
    vTaskDelete(NULL);
}

/* ---- Listener task ---------------------------------------------------- */

static void listener_task(void *arg)
{
    (void)arg;
    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_ANY),
        .sin_port        = htons(RIGCTLD_PORT),
    };

    s_listen_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s_listen_fd < 0) {
        ESP_LOGE(TAG, "socket() failed: errno=%d", errno);
        s_listen_task = NULL;
        vTaskDelete(NULL);
        return;
    }
    int one = 1;
    setsockopt(s_listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    if (bind(s_listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "bind() failed: errno=%d", errno);
        close(s_listen_fd); s_listen_fd = -1;
        s_listen_task = NULL;
        vTaskDelete(NULL);
        return;
    }
    if (listen(s_listen_fd, 4) < 0) {
        ESP_LOGE(TAG, "listen() failed: errno=%d", errno);
        close(s_listen_fd); s_listen_fd = -1;
        s_listen_task = NULL;
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "rigctld listening on tcp/%d", RIGCTLD_PORT);

    for (;;) {
        struct sockaddr_in cli;
        socklen_t cli_len = sizeof(cli);
        int cfd = accept(s_listen_fd, (struct sockaddr *)&cli, &cli_len);
        if (cfd < 0) {
            if (s_listen_fd < 0) break;     /* stopped */
            ESP_LOGW(TAG, "accept() errno=%d", errno);
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        if (s_client_count >= RIGCTLD_MAX_CLIENTS) {
            ESP_LOGW(TAG, "too many clients (%d), refusing fd=%d",
                     s_client_count, cfd);
            const char *msg = "RPRT -11\n";
            send(cfd, msg, strlen(msg), 0);
            close(cfd);
            continue;
        }

        s_client_count++;
        char name[24];
        snprintf(name, sizeof(name), "rig_cli_%d", cfd);
        BaseType_t ok = xTaskCreate(client_task, name, 8192,
                                    (void *)(intptr_t)cfd, 4, NULL);
        if (ok != pdPASS) {
            ESP_LOGE(TAG, "xTaskCreate %s failed", name);
            close(cfd);
            s_client_count--;
        }
    }

    if (s_listen_fd >= 0) close(s_listen_fd);
    s_listen_fd = -1;
    s_listen_task = NULL;
    vTaskDelete(NULL);
}

/* ---- Public API ------------------------------------------------------- */

esp_err_t rigctld_server_start(void)
{
    if (s_listen_task != NULL) return ESP_OK;

    BaseType_t ok = xTaskCreate(listener_task, "rigctld_lsn", 4096,
                                NULL, 4, &s_listen_task);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreate rigctld_lsn failed");
        s_listen_task = NULL;
        return ESP_FAIL;
    }
    return ESP_OK;
}

void rigctld_server_stop(void)
{
    if (s_listen_fd >= 0) {
        int fd = s_listen_fd;
        s_listen_fd = -1;
        shutdown(fd, SHUT_RDWR);
        close(fd);
    }
}
