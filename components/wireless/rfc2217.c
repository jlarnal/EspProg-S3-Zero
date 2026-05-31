/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "rfc2217.h"
#include "sdkconfig.h"
#if CONFIG_WIRELESS_SERIAL
#include <stdint.h>
#include <errno.h>
#include <sys/time.h>
#include "serial_handler.h"
#include "lwip/sockets.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_log.h"

#define CLIENT_SOCK_TIMEOUT_S 2   /* recv/send timeout so a stalled client can't wedge the task */

static const char *TAG = "rfc2217";
static int s_listen = -1, s_client = -1;
/* Set by the send path when the client stops draining (timeout/hard error); the
 * recv loop checks it after a recv timeout and tears the connection down so the
 * single-client server returns to accept() instead of wedging forever. */
static volatile bool s_client_err = false;

/* Telnet negotiation needs a quiet line. A target that streams data (a chatty
 * app, or boot-ROM garbage at 74880 baud read as 115200) buries the server's
 * option replies behind bulk data in the TCP send queue, so the client's open()
 * negotiation times out. Suppress target->client forwarding until the client
 * gets past negotiation: its first COM-PORT command (esptool, immediately after
 * telnet options) or a 1s fallback (plain telnet monitors that never send one). */
#define NEGOTIATION_QUIET_US (1000 * 1000)
static volatile bool s_negotiated = false;
static int64_t s_connect_us = 0;

/* Telnet / RFC2217 (COM-PORT-CONTROL option 44) constants */
#define IAC  255
#define DONT 254
#define DO   253
#define WONT 252
#define WILL 251
#define SB   250
#define SE   240
#define OPT_BINARY     0
#define OPT_SGA        3
#define COM_PORT_OPT  44
/* client->server COM-PORT commands (server confirms with cmd + 100) */
#define SET_BAUDRATE   1
#define SET_CONTROL    5
/* SET_CONTROL values we act on */
#define DTR_ON   8
#define DTR_OFF  9
#define RTS_ON  11
#define RTS_OFF 12

/* Per-connection state. */
typedef struct {
    bool    dtr, rts;
    uint8_t cmd;        /* pending DO/WILL/DONT/WONT while reading its option byte */
    uint8_t sb[32];
    int     sbn;
    int     state;      /* 0 data, 1 iac, 2 opt-byte, 3 sb, 4 sb-iac */
} tn_t;

static void sock_send(const uint8_t *b, int n)
{
    const int c = s_client;
    if (c >= 0 && n > 0) {
        send(c, b, n, 0);
    }
}

static void tn_opt_reply(uint8_t cmd, uint8_t opt)
{
    const uint8_t r[3] = { IAC, cmd, opt };
    sock_send(r, 3);
}

/* Echo a COM-PORT server confirmation (client command + 100) with the value bytes
 * the client sent. pyserial waits for these before it proceeds. */
static void tn_comport_reply(uint8_t client_cmd, const uint8_t *val, int vlen)
{
    uint8_t r[64];
    int o = 0;
    r[o++] = IAC;
    r[o++] = SB;
    r[o++] = COM_PORT_OPT;
    r[o++] = (uint8_t)(client_cmd + 100);
    for (int i = 0; i < vlen && o < (int)sizeof(r) - 3; i++) {
        r[o++] = val[i];
        if (val[i] == IAC) {
            r[o++] = IAC;   /* escape 0xFF inside the subnegotiation */
        }
    }
    r[o++] = IAC;
    r[o++] = SE;
    sock_send(r, o);
}

/* Postpone timer for the BOOT=1/RST=1 state (see apply_control). */
static esp_timer_handle_t s_br_timer;

static void br_timer_cb(void *arg)
{
    (void) arg;
    serial_handler_set_boot_reset_pins(true, true);   // BOOT=1, RST=1 (not in reset)
}

/* Mirror serial_bridge.c tud_cdc_line_state_cb mapping, INCLUDING its 10ms
 * postpone of the BOOT=1/RST=1 state. esptool sends DTR and RTS as separate
 * RFC2217 SET_CONTROL commands, so a (DTR=1,RTS=1) transient occurs between them
 * just like on USB; applying it immediately would release reset with BOOT high
 * and miss download mode. Postponing lets the following transition cancel it. */
static void apply_control(tn_t *t)
{
    bool rst = true, boot = true;
    if (!t->dtr && t->rts) {
        rst = false;
        boot = true;
    } else if (t->dtr && !t->rts) {
        rst = true;
        boot = false;
    }
    if (s_br_timer) {
        esp_timer_stop(s_br_timer);   // may not be running; ignore the result
    }
    if (t->dtr && t->rts) {
        if (s_br_timer) {
            esp_timer_start_once(s_br_timer, 10 * 1000 /* us */);
        }
    } else {
        ESP_LOGI(TAG, "DTR=%d RTS=%d -> BOOT=%d RST=%d", t->dtr, t->rts, boot, rst);
        serial_handler_set_boot_reset_pins(boot, rst);
        if (!rst) {
            serial_handler_set_baudrate(115200);
        }
    }
}

/* Send fully to the client, honoring SO_SNDTIMEO. On timeout or hard error,
 * flag the connection (don't block the caller) so server_task can recover. */
static void net_send(const uint8_t *b, int n)
{
    const int c = s_client;
    if (c < 0 || n <= 0) {
        return;
    }
    int off = 0;
    while (off < n) {
        const int r = send(c, b + off, n - off, 0);
        if (r > 0) {
            off += r;
            continue;
        }
        s_client_err = true;   /* EAGAIN (client not draining) or hard error */
        return;
    }
}

/* target -> socket (called by serial_handler when NET owns the UART) */
static void net_rx(const uint8_t *data, size_t len)
{
    if (s_client < 0) {
        return;
    }
    /* Hold off forwarding until the client is past telnet negotiation, so target
     * chatter can't bury the option replies and time out the client's open(). */
    if (!s_negotiated) {
        if (esp_timer_get_time() - s_connect_us < NEGOTIATION_QUIET_US) {
            return;
        }
        s_negotiated = true;
    }
    /* static: net_rx runs on the (single) serial_handler UART task; a 1KB stack
     * buffer there risks overflow. */
    static uint8_t tmp[1024];
    size_t o = 0;
    for (size_t i = 0; i < len && !s_client_err; i++) {
        if (o + 2 >= sizeof(tmp)) {
            net_send(tmp, o);
            o = 0;
        }
        tmp[o++] = data[i];
        if (data[i] == IAC) {
            tmp[o++] = IAC;   /* escape 0xFF */
        }
    }
    if (o && !s_client_err) {
        net_send(tmp, o);
    }
}

static void handle_subneg(tn_t *t)
{
    if (t->sbn < 2 || t->sb[0] != COM_PORT_OPT) {
        return;
    }
    const uint8_t cc = t->sb[1];
    if (cc == SET_CONTROL && t->sbn >= 3) {
        switch (t->sb[2]) {
        case DTR_ON:  t->dtr = true;  apply_control(t); break;
        case DTR_OFF: t->dtr = false; apply_control(t); break;
        case RTS_ON:  t->rts = true;  apply_control(t); break;
        case RTS_OFF: t->rts = false; apply_control(t); break;
        default: break;
        }
    } else if (cc == SET_BAUDRATE && t->sbn >= 6) {
        const uint32_t baud = ((uint32_t)t->sb[2] << 24) | ((uint32_t)t->sb[3] << 16) |
                              ((uint32_t)t->sb[4] << 8) | (uint32_t)t->sb[5];
        if (baud) {
            serial_handler_set_baudrate(baud);
        }
    }
    /* Confirm any client COM-PORT "set" command (1..12) by echoing it back with
     * the same value; pyserial blocks waiting for this. */
    if (cc >= 1 && cc <= 12) {
        s_negotiated = true;   /* client is past telnet negotiation -> resume forwarding */
        tn_comport_reply(cc, &t->sb[2], t->sbn - 2);
    }
}

/* socket -> target: handle telnet negotiation, forward payload to the UART. */
static void feed(tn_t *t, const uint8_t *buf, int n)
{
    /* static: called only from the (single) rfc2217 server task. */
    static uint8_t out[1024];
    int o = 0;
    for (int i = 0; i < n; i++) {
        const uint8_t b = buf[i];
        switch (t->state) {
        case 0:
            if (b == IAC) {
                t->state = 1;
            } else {
                out[o++] = b;
            }
            break;
        case 1:
            if (b == IAC) {
                out[o++] = IAC;          /* escaped 0xFF -> data */
                t->state = 0;
            } else if (b == SB) {
                t->sbn = 0;
                t->state = 3;
            } else if (b == DO || b == DONT || b == WILL || b == WONT) {
                t->cmd = b;
                t->state = 2;
            } else {
                t->state = 0;            /* other 2-byte command, ignore */
            }
            break;
        case 2: {
            const uint8_t opt = b;
            const bool supported = (opt == OPT_BINARY || opt == OPT_SGA || opt == COM_PORT_OPT);
            if (t->cmd == WILL) {
                tn_opt_reply(supported ? DO : DONT, opt);
            } else if (t->cmd == DO) {
                tn_opt_reply(supported ? WILL : WONT, opt);
            } else if (t->cmd == WONT) {
                tn_opt_reply(DONT, opt);
            } else { /* DONT */
                tn_opt_reply(WONT, opt);
            }
            t->state = 0;
            break;
        }
        case 3:
            if (b == IAC) {
                t->state = 4;
            } else if (t->sbn < (int)sizeof(t->sb)) {
                t->sb[t->sbn++] = b;
            }
            break;
        case 4:
            if (b == SE) {
                handle_subneg(t);
                t->state = 0;
            } else {                     /* IAC IAC inside SB -> literal 0xFF */
                if (t->sbn < (int)sizeof(t->sb)) {
                    t->sb[t->sbn++] = b;
                }
                t->state = 3;
            }
            break;
        default:
            t->state = 0;
            break;
        }
    }
    if (o > 0) {
        serial_handler_send_data(out, o);
    }
}

static void server_task(void *arg)
{
    const uint16_t port = (uint16_t)(uintptr_t)arg;
    s_listen = socket(AF_INET, SOCK_STREAM, 0);
    int yes = 1;
    setsockopt(s_listen, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    struct sockaddr_in a = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_ANY),
        .sin_port = htons(port),
    };
    if (bind(s_listen, (struct sockaddr *)&a, sizeof(a)) != 0 || listen(s_listen, 1) != 0) {
        ESP_LOGE(TAG, "bind/listen on :%u failed", port);
        close(s_listen);
        s_listen = -1;
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "RFC2217 listening on :%u", port);
    while (1) {
        struct sockaddr_in ca;
        socklen_t cl = sizeof(ca);
        const int c = accept(s_listen, (struct sockaddr *)&ca, &cl);
        if (c < 0) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        if (!serial_handler_acquire(SERIAL_OWNER_NET)) {
            ESP_LOGW(TAG, "UART busy (USB session) -> refusing client");
            close(c);
            continue;
        }
        /* Bounded recv/send + keepalive: a client that dies mid-session (or a
         * pyserial open() that times out without a clean FIN) must not leave this
         * single-client task blocked forever in recv()/send(), which previously
         * wedged the whole server (it never returned to accept()). */
        const struct timeval tv = { .tv_sec = CLIENT_SOCK_TIMEOUT_S, .tv_usec = 0 };
        setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        const int keepalive = 1;
        setsockopt(c, SOL_SOCKET, SO_KEEPALIVE, &keepalive, sizeof(keepalive));
        /* Detect a half-open/dead peer in ~7s (idle 4s, then 3 probes 1s apart),
         * so a client that vanishes without a FIN -- and without us sending data
         * to trip s_client_err -- still unblocks recv() and frees the server. */
        const int ka_idle = 4, ka_intvl = 1, ka_cnt = 3;
        setsockopt(c, IPPROTO_TCP, TCP_KEEPIDLE, &ka_idle, sizeof(ka_idle));
        setsockopt(c, IPPROTO_TCP, TCP_KEEPINTVL, &ka_intvl, sizeof(ka_intvl));
        setsockopt(c, IPPROTO_TCP, TCP_KEEPCNT, &ka_cnt, sizeof(ka_cnt));

        tn_t t = {0};
        s_client_err = false;
        s_negotiated = false;
        s_connect_us = esp_timer_get_time();
        s_client = c;
        serial_handler_register_net_data_callback(net_rx);
        ESP_LOGI(TAG, "client connected");
        /* Proactively offer the options pyserial wants, so negotiation completes
         * even if the client waits for the server to lead. */
        const uint8_t hello[] = {
            IAC, WILL, COM_PORT_OPT, IAC, DO, COM_PORT_OPT,
            IAC, WILL, OPT_BINARY,   IAC, DO, OPT_BINARY,
            IAC, WILL, OPT_SGA,      IAC, DO, OPT_SGA,
        };
        send(c, hello, sizeof(hello), 0);
        static uint8_t buf[1024];   /* single server task; keep off the stack */
        while (1) {
            const int n = recv(c, buf, sizeof(buf), 0);
            if (n > 0) {
                feed(&t, buf, n);
                continue;
            }
            if (n == 0) {
                break;                 /* peer closed cleanly */
            }
            /* n < 0: a recv timeout (EAGAIN/EWOULDBLOCK) is normal during idle —
             * keep the session unless the send path flagged the client dead. */
            if ((errno == EAGAIN || errno == EWOULDBLOCK) && !s_client_err) {
                continue;
            }
            break;                     /* dead client or hard recv error */
        }
        serial_handler_register_net_data_callback(NULL);
        s_client = -1;
        serial_handler_release(SERIAL_OWNER_NET);
        close(c);
        ESP_LOGI(TAG, "client %s", s_client_err ? "dropped (stalled)" : "disconnected");
    }
}

void rfc2217_start(uint16_t port)
{
    static bool started = false;
    if (started) {
        return;
    }
    started = true;
    const esp_timer_create_args_t ta = { .callback = br_timer_cb, .name = "rfc2217_br" };
    esp_timer_create(&ta, &s_br_timer);
    xTaskCreate(server_task, "rfc2217", 8192, (void *)(uintptr_t)port, 5, NULL);
}

void rfc2217_stop(void)
{
    if (s_client >= 0) {
        close(s_client);
        s_client = -1;
    }
    if (s_listen >= 0) {
        close(s_listen);
        s_listen = -1;
    }
}
#endif
