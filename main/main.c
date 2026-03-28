#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "esp_ota_ops.h"
#include "nvs_flash.h"
#include "usb/usb_host.h"
#include "riello_usb.h"
#include "ups_driver.h"
#include "nut_server.h"
#include "nvs_config.h"
#include "web_server.h"
#include "mqtt_pub.h"
#include "sdkconfig.h"

#ifdef CONFIG_APC_NUT_BOARD_ESP32S3
#include "esp_wifi.h"
#endif

#ifdef CONFIG_APC_NUT_BOARD_ESP32P4_ETH
#include "esp_eth.h"
#include "esp_eth_mac.h"
#include "esp_eth_phy.h"
#include "driver/gpio.h"
#endif

static const char *TAG = "main";

#define NET_CONNECTED_BIT BIT0
#define NET_FAIL_BIT      BIT1

static EventGroupHandle_t s_net_event_group;

static void on_got_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
    ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&e->ip_info.ip));
    xEventGroupSetBits(s_net_event_group, NET_CONNECTED_BIT);
}

/* ── WiFi (ESP32-S3) ───────────────────────────────────────────────────── */
#ifdef CONFIG_APC_NUT_BOARD_ESP32S3

#define WIFI_MAX_RETRIES 10
static int s_wifi_retry = 0;

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_wifi_retry < WIFI_MAX_RETRIES) {
            esp_wifi_connect();
            s_wifi_retry++;
            ESP_LOGW(TAG, "WiFi reconnecting (%d/%d)", s_wifi_retry, WIFI_MAX_RETRIES);
        } else {
            xEventGroupSetBits(s_net_event_group, NET_FAIL_BIT);
        }
    }
}

static esp_err_t network_init(void)
{
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        on_got_ip, NULL, NULL));

    wifi_config_t wifi_cfg = {
        .sta = {
            .ssid     = CONFIG_APC_NUT_WIFI_SSID,
            .password = CONFIG_APC_NUT_WIFI_PASSWORD,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Connecting to SSID: %s", CONFIG_APC_NUT_WIFI_SSID);

    EventBits_t bits = xEventGroupWaitBits(s_net_event_group,
                                           NET_CONNECTED_BIT | NET_FAIL_BIT,
                                           pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(30000));
    return (bits & NET_CONNECTED_BIT) ? ESP_OK : ESP_FAIL;
}
#endif

/* ── Ethernet / IP101 RMII (ESP32-P4-ETH) ─────────────────────────────── */
#ifdef CONFIG_APC_NUT_BOARD_ESP32P4_ETH

#define ETH_RMII_TXD0   35
#define ETH_RMII_TXD1   34
#define ETH_RMII_RXD0   29
#define ETH_RMII_RXD1   30
#define ETH_RMII_TX_EN  49
#define ETH_RMII_CRS_DV 28
#define ETH_RMII_REFCLK 50
#define ETH_MDC         31
#define ETH_MDIO        52
#define ETH_PHY_PWR     51
#define ETH_PHY_ADDR     1
#define USB_VBUS_EN     45  /* P-ch MOSFET Q1 gate — drive LOW to enable 5V VBUS on USB-A */

static void eth_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if      (id == ETHERNET_EVENT_CONNECTED)    ESP_LOGI(TAG, "Ethernet link up");
    else if (id == ETHERNET_EVENT_DISCONNECTED) ESP_LOGW(TAG, "Ethernet link down");
}

static esp_err_t network_init(void)
{
    gpio_config_t pwr_cfg = {
        .pin_bit_mask = BIT64(ETH_PHY_PWR),
        .mode         = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&pwr_cfg));
    gpio_set_level(ETH_PHY_PWR, 1);
    vTaskDelay(pdMS_TO_TICKS(10));

    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
    esp_netif_t *eth_netif = esp_netif_new(&netif_cfg);

    eth_mac_config_t mac_cfg = ETH_MAC_DEFAULT_CONFIG();
    eth_esp32_emac_config_t emac_cfg = ETH_ESP32_EMAC_DEFAULT_CONFIG();
    emac_cfg.smi_mdc_gpio_num  = ETH_MDC;
    emac_cfg.smi_mdio_gpio_num = ETH_MDIO;
    emac_cfg.interface         = EMAC_DATA_INTERFACE_RMII;
    emac_cfg.clock_config.rmii.clock_mode = EMAC_CLK_EXT_IN;
    emac_cfg.clock_config.rmii.clock_gpio = ETH_RMII_REFCLK;

    esp_eth_mac_t *mac = esp_eth_mac_new_esp32(&emac_cfg, &mac_cfg);

    eth_phy_config_t phy_cfg = ETH_PHY_DEFAULT_CONFIG();
    phy_cfg.phy_addr       = ETH_PHY_ADDR;
    phy_cfg.reset_gpio_num = -1;
    esp_eth_phy_t *phy = esp_eth_phy_new_ip101(&phy_cfg);

    esp_eth_config_t eth_cfg = ETH_DEFAULT_CONFIG(mac, phy);
    esp_eth_handle_t eth_handle = NULL;
    ESP_ERROR_CHECK(esp_eth_driver_install(&eth_cfg, &eth_handle));
    ESP_ERROR_CHECK(esp_netif_attach(eth_netif, esp_eth_new_netif_glue(eth_handle)));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(ETH_EVENT, ESP_EVENT_ANY_ID,
                                                        eth_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_ETH_GOT_IP,
                                                        on_got_ip, NULL, NULL));

    ESP_ERROR_CHECK(esp_eth_start(eth_handle));
    ESP_LOGI(TAG, "Ethernet starting (RMII, IP101, PHY addr %d)", ETH_PHY_ADDR);

    EventBits_t bits = xEventGroupWaitBits(s_net_event_group,
                                           NET_CONNECTED_BIT | NET_FAIL_BIT,
                                           pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(15000));
    return (bits & NET_CONNECTED_BIT) ? ESP_OK : ESP_FAIL;
}
#endif

/* ── USB host daemon ───────────────────────────────────────────────────── */
static void usb_host_task(void *arg)
{
    esp_task_wdt_add(NULL);
    while (1) {
        uint32_t event_flags;
        usb_host_lib_handle_events(pdMS_TO_TICKS(5000), &event_flags);
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS)
            usb_host_device_free_all();
        esp_task_wdt_reset();
    }
}

static void shutdown_mqtt(void) { mqtt_pub_stop(); }
static void shutdown_nut(void)  { nut_server_stop(); }
static void shutdown_web(void)  { web_server_stop(); }

/* ── Entry point ───────────────────────────────────────────────────────── */
void app_main(void)
{
    ESP_LOGI(TAG, "APC NUT Server starting");

    /* Mark this OTA partition as valid so the bootloader doesn't roll back */
    esp_ota_mark_app_valid_cancel_rollback();

    esp_task_wdt_config_t wdt_cfg = {
        .timeout_ms     = 30000,
        .idle_core_mask = 0,
        .trigger_panic  = true,
    };
    esp_err_t wdt_err = esp_task_wdt_reconfigure(&wdt_cfg);
    if (wdt_err == ESP_ERR_INVALID_STATE)
        ESP_ERROR_CHECK(esp_task_wdt_init(&wdt_cfg));

    esp_register_shutdown_handler(shutdown_mqtt);
    esp_register_shutdown_handler(shutdown_nut);
    esp_register_shutdown_handler(shutdown_web);

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(nvs_config_init());

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    s_net_event_group = xEventGroupCreate();

    if (network_init() != ESP_OK)
        ESP_LOGW(TAG, "Network unavailable — services will start anyway");

#ifdef CONFIG_APC_NUT_BOARD_ESP32P4_ETH
    /* Enable 5V VBUS on USB-A port: GPIO45 drives P-ch MOSFET Q1, active LOW */
    gpio_config_t vbus_cfg = {
        .pin_bit_mask = BIT64(USB_VBUS_EN),
        .mode         = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&vbus_cfg));
    gpio_set_level(USB_VBUS_EN, 0);
    vTaskDelay(pdMS_TO_TICKS(100));  /* let VBUS stabilise before host init */
    ESP_LOGI(TAG, "USB VBUS enabled (GPIO%d low)", USB_VBUS_EN);
#endif

    usb_host_config_t host_cfg = {
        .skip_phy_setup = false,
        .intr_flags     = ESP_INTR_FLAG_LEVEL1,
    };
    ESP_ERROR_CHECK(usb_host_install(&host_cfg));
    xTaskCreatePinnedToCore(usb_host_task, "usb_host", 4096, NULL, 5, NULL, 0);

    ESP_ERROR_CHECK(riello_usb_init());
    xTaskCreate(riello_usb_task, "riello_usb", 4096, NULL, 4, NULL);

    ESP_ERROR_CHECK(ups_driver_init());
    xTaskCreate(ups_driver_poll_task, "ups_poll", 4096, NULL, 4, NULL);

    ESP_ERROR_CHECK(nut_server_init());
    xTaskCreate(nut_server_task, "nut_server", 8192, NULL, 4, NULL);

    ESP_ERROR_CHECK(web_server_start());

    ESP_ERROR_CHECK(mqtt_pub_init());
    mqtt_pub_reconfigure();
    xTaskCreate(mqtt_pub_task, "mqtt_pub", 6144, NULL, 3, NULL);

    ESP_LOGI(TAG, "All tasks started");
}
