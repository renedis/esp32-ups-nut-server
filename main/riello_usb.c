/* ESP32 UPS NUT Server — Copyright (c) 2026 renedis — GPL-3.0 */

/*
 * Riello UPS — USB bulk-transfer driver for ESP32.
 *
 * Protocol: GPSER (Riello proprietary ASCII-framed protocol over USB bulk).
 * USB class: HID (0x03), but communication uses bulk EP 0x02 (OUT) / 0x81 (IN)
 * with 8-byte frames instead of standard HID interrupt/feature reports.
 *
 * Reference implementation: NUT drivers/riello_usb.c + riello.c
 * Protocol spec: https://www.networkupstools.org/protocols/riello/PSGPSER-0104.pdf
 */

#include "riello_usb.h"
#include "ups_driver.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "usb/usb_host.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "riello";

/* -----------------------------------------------------------------------
 * Device constants
 * ----------------------------------------------------------------------- */
#define RIELLO_VID          0x04b4u
#define RIELLO_PID          0x5500u
#define RIELLO_IFACE        0           /* USB interface number */
#define EP_BULK_OUT         0x02u       /* Bulk OUT endpoint */
#define EP_BULK_IN          0x81u       /* Bulk IN endpoint  */

#define FRAME_SIZE          8           /* USB bulk frame bytes      */
#define PAYLOAD_PER_FRAME   7           /* Data bytes per frame      */
#define BUF_SIZE            220         /* Max protocol buffer       */

/* GPSER response lengths (payload bytes, excl. CRC and ETX) */
#define LENGTH_GI           68
#define LENGTH_RS_MM        42          /* Mono-phase RS response    */

#define POLL_INTERVAL_MS    5000

/* -----------------------------------------------------------------------
 * Runtime state
 * ----------------------------------------------------------------------- */
static usb_host_client_handle_t s_client    = NULL;
static usb_device_handle_t      s_dev       = NULL;
static bool                     s_connected = false;

/* Pending events (set in callback, processed in task loop) */
static volatile uint8_t  s_pending_addr       = 0;
static volatile bool     s_pending_connect    = false;
static volatile bool     s_pending_disconnect = false;

/* Transfer objects */
static usb_transfer_t *s_xfer_out = NULL;   /* reused for OUT and ctrl */
static usb_transfer_t *s_xfer_in  = NULL;

/* Transfer completion signalling (polled inside wait_xfer) */
static volatile bool                 s_xfer_done   = false;
static volatile usb_transfer_status_t s_xfer_status = USB_TRANSFER_STATUS_COMPLETED;

/* -----------------------------------------------------------------------
 * Transfer callback — called from within usb_host_client_handle_events()
 * ----------------------------------------------------------------------- */
static void xfer_cb(usb_transfer_t *t)
{
    s_xfer_status = t->status;
    s_xfer_done   = true;
}

/* -----------------------------------------------------------------------
 * Pump the event loop until current transfer completes (or timeout).
 *
 * This is safe to call from within riello_usb_task because
 * usb_host_client_handle_events() dispatches transfer completions —
 * the callback fires synchronously inside the call, setting s_xfer_done.
 * ----------------------------------------------------------------------- */
static esp_err_t wait_xfer(uint32_t timeout_ms)
{
    uint32_t elapsed = 0;
    while (!s_xfer_done && elapsed < timeout_ms) {
        usb_host_client_handle_events(s_client, pdMS_TO_TICKS(10));
        elapsed += 10;
    }
    s_xfer_done = false;
    if (elapsed >= timeout_ms) {
        ESP_LOGE(TAG, "transfer timeout");
        return ESP_ERR_TIMEOUT;
    }
    if (s_xfer_status != USB_TRANSFER_STATUS_COMPLETED) {
        ESP_LOGW(TAG, "transfer status %d", (int)s_xfer_status);
        return ESP_FAIL;
    }
    return ESP_OK;
}

/* -----------------------------------------------------------------------
 * CRC (GPSER polynomial, ported from NUT riello.c)
 *
 *   Initial value: 0x554D
 *   Algorithm: CRC-16/RIELLO (custom)
 *   Encoded as 4 ASCII hex characters appended to the frame.
 * ----------------------------------------------------------------------- */
static uint16_t gpser_crc(const uint8_t *buf, uint16_t size)
{
    uint16_t pom, crc = 0x554D;
    buf++;          /* skip STX byte */
    size--;
    while (size--) {
        pom = (crc ^ *buf) & 0x00ff;
        pom = (pom ^ (pom << 4)) & 0x00ff;
        pom = (pom << 8) ^ (pom << 3) ^ (pom >> 4);
        crc = (crc >> 8) ^ pom;
        buf++;
    }
    return crc;
}

static void append_crc(uint8_t *buf, uint16_t *len)
{
    uint16_t crc = gpser_crc(buf, *len);
    buf[(*len)++] = (uint8_t)((crc / 4096)              + 0x30);
    buf[(*len)++] = (uint8_t)(((crc % 4096) / 256)      + 0x30);
    buf[(*len)++] = (uint8_t)((((crc % 4096) % 256) / 16) + 0x30);
    buf[(*len)++] = (uint8_t)((((crc % 4096) % 256) % 16) + 0x30);
}

/* -----------------------------------------------------------------------
 * StatusCode bit test.
 *
 * StatusCode is 5 ASCII hex chars representing a 20-bit packed value.
 * Bit N lives in hex digit (N/4), at bit position (N%4) within that digit.
 * ----------------------------------------------------------------------- */
static uint8_t status_bit(const uint8_t *sc, int bit)
{
    uint8_t hd = sc[bit / 4];
    uint8_t v  = (hd >= 'A') ? (hd - 'A' + 10) : (hd - '0');
    return (v >> (bit % 4)) & 1u;
}

/* -----------------------------------------------------------------------
 * USB frame send/receive
 *
 * Riello encodes payload into 8-byte USB bulk frames:
 *   OUT frame: [0x37 | 0x30+rem] [up to 7 payload bytes]
 *   IN  frame: [size_indicator & 0x07] [up to 7 payload bytes]
 * ----------------------------------------------------------------------- */
static esp_err_t bulk_send(const uint8_t *data, uint16_t len)
{
    uint16_t off = 0;

    while (off < len) {
        uint16_t chunk = (len - off >= PAYLOAD_PER_FRAME) ? PAYLOAD_PER_FRAME
                                                           : (len - off);
        uint8_t *fb = s_xfer_out->data_buffer;
        memset(fb, '0', FRAME_SIZE);
        fb[0] = (chunk == PAYLOAD_PER_FRAME) ? 0x37 : (0x30 + chunk);
        memcpy(&fb[1], &data[off], chunk);

        s_xfer_out->bEndpointAddress = EP_BULK_OUT;
        s_xfer_out->num_bytes        = FRAME_SIZE;
        s_xfer_out->callback         = xfer_cb;
        s_xfer_out->context          = NULL;

        if (usb_host_transfer_submit(s_xfer_out) != ESP_OK) return ESP_FAIL;
        if (wait_xfer(1000) != ESP_OK) return ESP_FAIL;
        off += chunk;
    }
    return ESP_OK;
}

static esp_err_t bulk_recv(uint8_t *rxbuf, uint16_t *rxlen, uint16_t expected)
{
    *rxlen = 0;
    /* Guard: don't loop forever if device stops sending */
    uint8_t attempts = 0;

    while (*rxlen < expected && attempts < 64) {
        s_xfer_in->bEndpointAddress = EP_BULK_IN;
        s_xfer_in->num_bytes        = FRAME_SIZE;
        s_xfer_in->callback         = xfer_cb;
        s_xfer_in->context          = NULL;

        if (usb_host_transfer_submit(s_xfer_in) != ESP_OK) return ESP_FAIL;
        if (wait_xfer(1000) != ESP_OK) return ESP_FAIL;

        uint8_t payload = s_xfer_in->data_buffer[0] & 0x07u;
        if (payload > 0) {
            uint16_t copy = payload;
            if (*rxlen + copy > BUF_SIZE) copy = BUF_SIZE - *rxlen;
            memcpy(&rxbuf[*rxlen], &s_xfer_in->data_buffer[1], copy);
            *rxlen += payload;
        }
        attempts++;
    }
    return (*rxlen >= expected) ? ESP_OK : ESP_FAIL;
}

/* -----------------------------------------------------------------------
 * Cypress initialisation: HID SET_REPORT (feature, 5 bytes)
 * This is required once after connect to wake the USB interface.
 * ----------------------------------------------------------------------- */
static esp_err_t cypress_init(void)
{
    usb_setup_packet_t *setup = (usb_setup_packet_t *)s_xfer_out->data_buffer;
    setup->bmRequestType = 0x21;           /* Class | Host→Device | Interface */
    setup->bRequest      = 0x09;           /* HID SET_REPORT */
    setup->wValue        = (0x03u << 8);   /* Feature report type */
    setup->wIndex        = 0;
    setup->wLength       = 5;

    uint8_t *payload = s_xfer_out->data_buffer + sizeof(usb_setup_packet_t);
    payload[0] = 0xB0;
    payload[1] = 0x04;
    payload[2] = 0x00;
    payload[3] = 0x00;
    payload[4] = 0x03;

    s_xfer_out->num_bytes = (int)(sizeof(usb_setup_packet_t) + 5);
    s_xfer_out->callback  = xfer_cb;
    s_xfer_out->context   = NULL;

    esp_err_t ret = usb_host_transfer_submit_control(s_client, s_xfer_out);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "cypress_init submit: %s", esp_err_to_name(ret));
        return ret;
    }
    return wait_xfer(2000);
}

/* -----------------------------------------------------------------------
 * Command builders (GPSER protocol)
 * Format: STX(0x02) + 0x20 + 0x22 + CMD[2] + PARAM[2] + CRC[4] + ETX(0x03)
 * ----------------------------------------------------------------------- */
static uint16_t build_cmd(uint8_t *buf, char c1, char c2)
{
    uint16_t n = 0;
    buf[n++] = 0x02;  /* STX */
    buf[n++] = 0x20;
    buf[n++] = 0x22;
    buf[n++] = (uint8_t)c1;
    buf[n++] = (uint8_t)c2;
    buf[n++] = '0';
    buf[n++] = '0';
    append_crc(buf, &n);
    buf[n++] = 0x03;  /* ETX */
    return n;
}

/* -----------------------------------------------------------------------
 * Inline helpers for ASCII-hex decoding
 * ----------------------------------------------------------------------- */
static inline uint16_t ahex2(const uint8_t *p)
{
    return (uint16_t)(p[0] - 0x30) * 16u + (p[1] - 0x30);
}
static inline uint16_t ahex3(const uint8_t *p)
{
    return (uint16_t)(p[0] - 0x30) * 256u + (uint16_t)(p[1] - 0x30) * 16u
           + (p[2] - 0x30);
}
static inline uint16_t ahex4(const uint8_t *p)
{
    return (uint16_t)(p[0] - 0x30) * 4096u + (uint16_t)(p[1] - 0x30) * 256u
           + (uint16_t)(p[2] - 0x30) * 16u  + (p[3] - 0x30);
}

/* -----------------------------------------------------------------------
 * Parse RS (real-time status) response — single-phase layout.
 *
 * All fields start at j=7 (after the 7-byte echoed command header).
 * Values are ASCII-hex encoded:
 *   j+ 0..4  : StatusCode[5]  (20-bit packed status flags)
 *   j+ 5..7  : Finp           (÷10 → Hz)
 *   j+ 8..10 : Uinp1          (V)
 *   j+11..13 : Fout           (÷10 → Hz)
 *   j+14..16 : Uout1          (V)
 *   j+17..18 : Pout1          (%)
 *   j+19..21 : Fbypass        (÷10 → Hz)
 *   j+22..24 : Ubypass1       (V)
 *   j+25..28 : Ubat           (÷100 → V, unit = 10 mV)
 *   j+29..30 : BatCap         (%)
 *   j+31..33 : BatTime        (minutes; 0xfff = unknown)
 *   j+34..35 : Tsystem        (°C)
 *
 * Note: byte offsets need verification against real hardware.
 * ----------------------------------------------------------------------- */
static void parse_rs(const uint8_t *buf)
{
    char tmp[32];
    const uint8_t *j = buf + 7;
    const uint8_t *sc = j;           /* StatusCode[5] */

    /* ups.status — built from status bits */
    char status[64] = "";
    if (status_bit(sc, 3))            /* output present */
        strlcat(status, "OL", sizeof(status));
    else
        strlcat(status, "OB", sizeof(status));
    if (status_bit(sc, 9)) strlcat(status, " BOOST", sizeof(status));
    if (status_bit(sc, 8)) strlcat(status, " TRIM",  sizeof(status));
    ups_driver_set_var("ups.status", status);

    /* input.frequency */
    snprintf(tmp, sizeof(tmp), "%.1f", ahex3(j + 5) / 10.0f);
    ups_driver_set_var("input.frequency", tmp);

    /* input.voltage */
    snprintf(tmp, sizeof(tmp), "%u", ahex3(j + 8));
    ups_driver_set_var("input.voltage", tmp);

    /* output.frequency */
    snprintf(tmp, sizeof(tmp), "%.1f", ahex3(j + 11) / 10.0f);
    ups_driver_set_var("output.frequency", tmp);

    /* output.voltage */
    snprintf(tmp, sizeof(tmp), "%u", ahex3(j + 14));
    ups_driver_set_var("output.voltage", tmp);

    /* ups.load */
    snprintf(tmp, sizeof(tmp), "%u", ahex2(j + 17));
    ups_driver_set_var("ups.load", tmp);

    /* battery.voltage (unit: 10 mV → V) */
    snprintf(tmp, sizeof(tmp), "%.2f", ahex4(j + 25) / 100.0f);
    ups_driver_set_var("battery.voltage", tmp);

    /* battery.charge */
    snprintf(tmp, sizeof(tmp), "%u", ahex2(j + 29));
    ups_driver_set_var("battery.charge", tmp);

    /* battery.runtime (minutes → seconds; 0xfff = not available) */
    uint16_t bat_time = ahex3(j + 31);
    if (bat_time != 0xfff) {
        snprintf(tmp, sizeof(tmp), "%u", (unsigned)(bat_time * 60u));
        ups_driver_set_var("battery.runtime", tmp);
    }

    /* ups.temperature */
    snprintf(tmp, sizeof(tmp), "%u", ahex2(j + 34));
    ups_driver_set_var("ups.temperature", tmp);
}

/* -----------------------------------------------------------------------
 * Parse GI (identification) response.
 *
 * j=7..24  : Identification[18] (ASCII, space-padded)
 * j=25..42 : ModelStr[18]
 * j=43..56 : Version[14]
 *
 * Offsets need hardware verification.
 * ----------------------------------------------------------------------- */
static void parse_gi(const uint8_t *buf)
{
    char model[19] = {0};
    char ver[15]   = {0};

    memcpy(model, buf + 25, 18);
    memcpy(ver,   buf + 43, 14);

    /* Trim trailing spaces */
    for (int i = 17; i >= 0 && (model[i] == ' ' || model[i] == '\0'); i--)
        model[i] = '\0';
    for (int i = 13; i >= 0 && (ver[i] == ' ' || ver[i] == '\0'); i--)
        ver[i] = '\0';

    ups_driver_set_var("device.mfr", "Riello");
    if (model[0]) ups_driver_set_var("device.model", model);
    if (ver[0])   ups_driver_set_var("ups.firmware",  ver);
}

/* -----------------------------------------------------------------------
 * High-level command: RS (real-time status poll)
 * ----------------------------------------------------------------------- */
static void send_rs(void)
{
    uint8_t  cmd[32];
    uint8_t  rxbuf[BUF_SIZE];
    uint16_t rxlen;
    uint16_t cmdlen = build_cmd(cmd, 'R', 'S');

    if (bulk_send(cmd, cmdlen) != ESP_OK) {
        ESP_LOGW(TAG, "RS: send failed");
        return;
    }
    if (bulk_recv(rxbuf, &rxlen, LENGTH_RS_MM) != ESP_OK) {
        ESP_LOGW(TAG, "RS: recv failed (got %u bytes)", rxlen);
        return;
    }
    parse_rs(rxbuf);
}

/* -----------------------------------------------------------------------
 * High-level command: GI (identification, read once at connect)
 * ----------------------------------------------------------------------- */
static void send_gi(void)
{
    uint8_t  cmd[32];
    uint8_t  rxbuf[BUF_SIZE];
    uint16_t rxlen;
    uint16_t cmdlen = build_cmd(cmd, 'G', 'I');

    if (bulk_send(cmd, cmdlen) != ESP_OK) return;
    if (bulk_recv(rxbuf, &rxlen, LENGTH_GI) != ESP_OK) return;
    parse_gi(rxbuf);
}

/* -----------------------------------------------------------------------
 * Device connect / disconnect
 * ----------------------------------------------------------------------- */
static void riello_connect(uint8_t dev_addr)
{
    /* Open device — check VID:PID */
    if (usb_host_device_open(s_client, dev_addr, &s_dev) != ESP_OK) {
        ESP_LOGE(TAG, "device_open failed");
        return;
    }
    const usb_device_desc_t *desc;
    usb_host_get_device_descriptor(s_dev, &desc);
    if (desc->idVendor != RIELLO_VID || desc->idProduct != RIELLO_PID) {
        usb_host_device_close(s_client, s_dev);
        s_dev = NULL;
        return;
    }
    ESP_LOGI(TAG, "Riello %04x:%04x found", desc->idVendor, desc->idProduct);

    /* Claim interface 0 */
    if (usb_host_interface_claim(s_client, s_dev, RIELLO_IFACE, 0) != ESP_OK) {
        ESP_LOGE(TAG, "interface_claim failed — HID host may have grabbed it");
        usb_host_device_close(s_client, s_dev);
        s_dev = NULL;
        return;
    }

    /* Allocate transfer buffers */
    if (!s_xfer_out)
        usb_host_transfer_alloc(sizeof(usb_setup_packet_t) + BUF_SIZE, 0, &s_xfer_out);
    if (!s_xfer_in)
        usb_host_transfer_alloc(FRAME_SIZE, 0, &s_xfer_in);
    s_xfer_out->device_handle = s_dev;
    s_xfer_in->device_handle  = s_dev;

    /* Wake up the Cypress USB bridge */
    if (cypress_init() != ESP_OK)
        ESP_LOGW(TAG, "Cypress init failed — will retry on next poll");

    s_connected = true;
    ups_driver_set_var("ups.status", "OL");

    /* Read identification once */
    send_gi();

    const char *model = ups_driver_get_var("device.model");
    ESP_LOGI(TAG, "Riello ready — %s", model ? model : "?");
}

static void riello_disconnect(void)
{
    ESP_LOGI(TAG, "Riello disconnected");
    s_connected = false;
    ups_driver_set_var("ups.status", "OFF");

    usb_host_interface_release(s_client, s_dev, RIELLO_IFACE);
    usb_host_device_close(s_client, s_dev);
    s_dev = NULL;
}

/* -----------------------------------------------------------------------
 * USB client event callback
 * NOTE: Called from within usb_host_client_handle_events().
 * Do NOT call any usb_host_* functions here — set flags only.
 * ----------------------------------------------------------------------- */
static void client_event_cb(const usb_host_client_event_msg_t *msg, void *arg)
{
    if (msg->event == USB_HOST_CLIENT_EVENT_NEW_DEV) {
        s_pending_addr    = msg->new_dev.address;
        s_pending_connect = true;
    } else if (msg->event == USB_HOST_CLIENT_EVENT_DEV_GONE) {
        if (s_connected && msg->dev_gone.dev_hdl == s_dev)
            s_pending_disconnect = true;
    }
}

/* -----------------------------------------------------------------------
 * Public API
 * ----------------------------------------------------------------------- */
esp_err_t riello_usb_init(void)
{
    usb_host_client_config_t cfg = {
        .is_synchronous    = false,
        .max_num_event_msg = 5,
        .async = {
            .client_event_callback = client_event_cb,
            .callback_arg          = NULL,
        },
    };
    esp_err_t ret = usb_host_client_register(&cfg, &s_client);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "client_register: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "Riello USB client registered (VID=%04x PID=%04x)",
             RIELLO_VID, RIELLO_PID);
    return ESP_OK;
}

void riello_usb_task(void *arg)
{
    esp_task_wdt_add(NULL);
    TickType_t last_poll = 0;

    while (1) {
        /* Pump USB client events — dispatches connect/disconnect/transfer callbacks */
        usb_host_client_handle_events(s_client, pdMS_TO_TICKS(100));
        esp_task_wdt_reset();

        /* Process pending connection events (outside callback context) */
        if (s_pending_connect) {
            s_pending_connect = false;
            riello_connect(s_pending_addr);
        }
        if (s_pending_disconnect) {
            s_pending_disconnect = false;
            riello_disconnect();
        }

        /* Periodic real-time status poll */
        if (s_connected) {
            TickType_t now = xTaskGetTickCount();
            if ((now - last_poll) >= pdMS_TO_TICKS(POLL_INTERVAL_MS)) {
                send_rs();
                last_poll = now;
            }
        }
    }
}
