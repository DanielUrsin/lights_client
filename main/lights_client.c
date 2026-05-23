#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "nvs_flash.h"

#include "esp_http_client.h"
#include "driver/gpio.h"

#define WIFI_SSID "lights"
#define WIFI_PASS "123456789"

#define LED_GPIO GPIO_NUM_8

#define SERVER_URL "http://192.168.4.1/lights"

static const char *TAG = "lights_client";

// Event bit set only when we have a usable IP address.
static EventGroupHandle_t wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0

// Local copy of server light state.
static uint32_t light_state = 0;

// Reconnect state.
static volatile bool reconnect_requested = false;
static int retry_count = 0;

/*
 * Wi-Fi / IP event handler.
 *
 * Important rule:
 * - WIFI_EVENT_STA_CONNECTED only means Wi-Fi association succeeded.
 * - IP_EVENT_STA_GOT_IP means networking is actually usable.
 */
static void wifi_event_handler(void *arg,
                               esp_event_base_t event_base,
                               int32_t event_id,
                               void *event_data)
{
    if (event_base == WIFI_EVENT &&
        event_id == WIFI_EVENT_STA_START) {

        ESP_LOGI(TAG, "Wi-Fi started, connecting...");

        reconnect_requested = false;
        esp_wifi_connect();
    }

    else if (event_base == WIFI_EVENT &&
             event_id == WIFI_EVENT_STA_CONNECTED) {

        ESP_LOGI(TAG, "Wi-Fi associated, waiting for IP...");
    }

    else if (event_base == WIFI_EVENT &&
             event_id == WIFI_EVENT_STA_DISCONNECTED) {

        wifi_event_sta_disconnected_t *event =
            (wifi_event_sta_disconnected_t *)event_data;

        ESP_LOGW(TAG,
                 "Disconnected from AP, reason=%d",
                 event->reason);

        // Clear connected bit because HTTP is no longer safe.
        xEventGroupClearBits(wifi_event_group, WIFI_CONNECTED_BIT);

        // Do not reconnect directly inside the event handler.
        // Let the reconnect task do it with controlled backoff.
        reconnect_requested = true;
    }

    else if (event_base == IP_EVENT &&
             event_id == IP_EVENT_STA_GOT_IP) {

        ip_event_got_ip_t *event =
            (ip_event_got_ip_t *)event_data;

        ESP_LOGI(TAG,
                 "Got IP: " IPSTR,
                 IP2STR(&event->ip_info.ip));

        retry_count = 0;
        reconnect_requested = false;

        // Mark Wi-Fi + IP as usable.
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        char *buffer = (char *)evt->user_data;

        int len = evt->data_len;
        if (len > 15) {
            len = 15;
        }

        memcpy(buffer, evt->data, len);
        buffer[len] = '\0';
    }

    return ESP_OK;
}


/*
 * Reconnect task.
 *
 * Handles reconnect attempts outside the Wi-Fi event handler.
 * This avoids aggressive retry loops that can cause WPA handshake instability.
 */
static void wifi_reconnect_task(void *arg)
{
    while (true) {
        if (reconnect_requested) {

            // Exponential-ish backoff:
            // 1s, 2s, 4s, 8s, then max 10s.
            int delay_ms = 1000 << retry_count;
            if (delay_ms > 10000) {
                delay_ms = 10000;
            }

            ESP_LOGI(TAG,
                     "Reconnect attempt %d in %d ms",
                     retry_count + 1,
                     delay_ms);

            vTaskDelay(pdMS_TO_TICKS(delay_ms));

            reconnect_requested = false;
            retry_count++;

            ESP_LOGI(TAG, "Reconnecting...");
            esp_wifi_connect();
        }

        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

/*
 * Initialize Wi-Fi station mode.
 *
 * Major steps:
 * 1. Initialize network stack
 * 2. Create default STA interface
 * 3. Set unique hostname
 * 4. Initialize Wi-Fi driver
 * 5. Register event handlers
 * 6. Configure SSID/password
 * 7. Disable power save
 * 8. Start Wi-Fi
 */
static void init_wifi_sta(void)
{
    wifi_event_group = xEventGroupCreate();

    // Initialize TCP/IP networking stack.
    ESP_ERROR_CHECK(esp_netif_init());

    // Create default event loop used by Wi-Fi and IP events.
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // Create default Wi-Fi station interface.
    esp_netif_t *netif = esp_netif_create_default_wifi_sta();

    // Generate stable hostname from MAC address.
    uint8_t mac[6];
    ESP_ERROR_CHECK(esp_read_mac(mac, ESP_MAC_WIFI_STA));

    char hostname[32];
    snprintf(hostname,
             sizeof(hostname),
             "esp32-%02X%02X%02X",
             mac[3],
             mac[4],
             mac[5]);

    ESP_ERROR_CHECK(esp_netif_set_hostname(netif, hostname));

    ESP_LOGI(TAG, "Hostname: %s", hostname);

    // Initialize Wi-Fi driver.
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // Register Wi-Fi event handler.
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT,
        ESP_EVENT_ANY_ID,
        wifi_event_handler,
        NULL,
        NULL
    ));

    // Register IP event handler.
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT,
        IP_EVENT_STA_GOT_IP,
        wifi_event_handler,
        NULL,
        NULL
    ));

    // Zero-initialize Wi-Fi config to avoid garbage in unused fields.
    wifi_config_t wifi_config = {};

    // Configure STA SSID.
    strncpy((char *)wifi_config.sta.ssid,
            WIFI_SSID,
            sizeof(wifi_config.sta.ssid) - 1);

    // Configure STA password.
    strncpy((char *)wifi_config.sta.password,
            WIFI_PASS,
            sizeof(wifi_config.sta.password) - 1);

    // Require at least WPA2.
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    // Put Wi-Fi into station/client mode.
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    // Apply station configuration.
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW20));

    // Disable Wi-Fi power saving for better ESP-to-ESP stability.
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    // Start reconnect task before Wi-Fi starts.
    xTaskCreate(
        wifi_reconnect_task,
        "wifi_reconnect",
        4096,
        NULL,
        5,
        NULL
    );

    // Start Wi-Fi. This triggers WIFI_EVENT_STA_START.
    ESP_ERROR_CHECK(esp_wifi_start());
}

/*
 * Initialize LED GPIO.
 */
static void init_gpio(void)
{
    gpio_reset_pin(LED_GPIO);
    gpio_set_direction(LED_GPIO, GPIO_MODE_OUTPUT);
}

/*
 * Fetch light state from the AP/server.
 *
 * GET http://192.168.4.1/lights
 *
 * Expected response:
 * "0" or "1"
 */
static bool fetch_light_state(uint32_t *out_state)
{
    char buffer[16] = {0};

    esp_http_client_config_t config = {
        .url = "http://192.168.4.1/lights",
        .timeout_ms = 3000,
        .event_handler = http_event_handler,
        .user_data = buffer,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);

    esp_err_t err = esp_http_client_perform(client);

    int status = esp_http_client_get_status_code(client);

    ESP_LOGI(TAG,
             "HTTP err=%s status=%d body='%s'",
             esp_err_to_name(err),
             status,
             buffer);

    esp_http_client_cleanup(client);

    if (err != ESP_OK || status != 200 || buffer[0] == '\0') {
        return false;
    }

    *out_state = atoi(buffer) > 0 ? 1 : 0;
    return true;
}

/*
 * Application entry point.
 */
void app_main(void)
{
    // Initialize NVS.
    // Wi-Fi requires NVS for calibration/config data.
    esp_err_t ret = nvs_flash_init();

    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    } else {
        ESP_ERROR_CHECK(ret);
    }

    // Initialize LED GPIO.
    init_gpio();

    // Initialize and start Wi-Fi station mode.
    init_wifi_sta();

    while (true) {

        // Wait until Wi-Fi has a valid IP address.
        xEventGroupWaitBits(
            wifi_event_group,
            WIFI_CONNECTED_BIT,
            pdFALSE,
            pdFALSE,
            portMAX_DELAY
        );

        // Poll server for light state.
        uint32_t new_state;
        if (fetch_light_state(&new_state)) {
            light_state = new_state;
        }

        // GPIO8 onboard LED is commonly active-low.
        // If your LED behaves inverted, remove the "!".
        gpio_set_level(LED_GPIO, !light_state);

        // Poll every 5 seconds.
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}
