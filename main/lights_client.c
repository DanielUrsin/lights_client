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
#include "esp_now.h"
#include "esp_sleep.h"
#include "nvs_flash.h"
#include "nvs.h"

#include "esp_http_client.h"
#include "driver/gpio.h"

#define WIFI_SSID "lights"
#define WIFI_PASS "123456789"

#define ONBOARD_LED_GPIO GPIO_NUM_8
#define EXTERNAL_LIGHTS_GPIO GPIO_NUM_10

#define SERVER_URL "http://192.168.4.1/lights"
#define SERVER_MAC_URL "http://192.168.4.1/mac"

#define ESP_NOW_GET_LIGHT_STATE "GET_LIGHT_STATE"
#define ESP_NOW_LIGHT_STATE_PREFIX "LIGHT_STATE="
#define ESP_NOW_MAX_RETRIES 3

#define REQUEST_INTERVAL_US (10ULL * 1000ULL * 1000ULL)
#define WIFI_CONNECT_TIMEOUT_MS 15000

#define NVS_NAMESPACE "lights"
#define NVS_KEY_LIGHT_STATE "state"
#define NVS_KEY_SERVER_MAC "server_mac"

static const char *TAG = "lights_client";

// Event bit set only when we have a usable IP address.
static EventGroupHandle_t wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0

// Event bit set when an ESP-NOW light state reply arrives.
static EventGroupHandle_t esp_now_event_group;
#define ESP_NOW_RESPONSE_BIT BIT0

// Local copy of server light state.
static uint32_t light_state = 0;
static bool light_state_persisted = false;

// ESP-NOW server peer state.
static uint8_t server_mac[ESP_NOW_ETH_ALEN] = {0};
static bool server_mac_known = false;
static bool esp_now_ready = false;
static uint32_t esp_now_reply_state = 0;

// Reconnect state.
static volatile bool reconnect_requested = false;
static int retry_count = 0;

typedef struct {
    char *data;
    size_t size;
    size_t len;
} http_response_buffer_t;

static bool load_persistent_state(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);

    if (err != ESP_OK) {
        ESP_LOGI(TAG, "No persisted state yet: %s", esp_err_to_name(err));
        return false;
    }

    uint8_t stored_state = 0;
    if (nvs_get_u8(handle, NVS_KEY_LIGHT_STATE, &stored_state) == ESP_OK) {
        light_state = stored_state > 0 ? 1 : 0;
        light_state_persisted = true;
    }

    size_t mac_len = ESP_NOW_ETH_ALEN;
    err = nvs_get_blob(handle, NVS_KEY_SERVER_MAC, server_mac, &mac_len);
    if (err == ESP_OK && mac_len == ESP_NOW_ETH_ALEN) {
        server_mac_known = true;
        ESP_LOGI(TAG,
                 "Loaded server MAC: %02X:%02X:%02X:%02X:%02X:%02X",
                 server_mac[0],
                 server_mac[1],
                 server_mac[2],
                 server_mac[3],
                 server_mac[4],
                 server_mac[5]);
    }

    nvs_close(handle);
    return server_mac_known;
}

static bool save_light_state(uint32_t state)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open failed while saving state: %s",
                 esp_err_to_name(err));
        return false;
    }

    err = nvs_set_u8(handle, NVS_KEY_LIGHT_STATE, state > 0 ? 1 : 0);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }

    nvs_close(handle);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Saving light state failed: %s", esp_err_to_name(err));
        return false;
    }

    light_state_persisted = true;
    return true;
}

static bool save_server_mac(const uint8_t mac[ESP_NOW_ETH_ALEN])
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open failed while saving MAC: %s",
                 esp_err_to_name(err));
        return false;
    }

    err = nvs_set_blob(handle, NVS_KEY_SERVER_MAC, mac, ESP_NOW_ETH_ALEN);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }

    nvs_close(handle);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Saving server MAC failed: %s", esp_err_to_name(err));
        return false;
    }

    return true;
}

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
        http_response_buffer_t *buffer =
            (http_response_buffer_t *)evt->user_data;

        if (buffer == NULL || buffer->data == NULL || buffer->size == 0) {
            return ESP_OK;
        }

        size_t available = buffer->size - buffer->len - 1;
        size_t copy_len = evt->data_len;

        if (copy_len > available) {
            copy_len = available;
        }

        if (copy_len > 0) {
            memcpy(buffer->data + buffer->len, evt->data, copy_len);
            buffer->len += copy_len;
            buffer->data[buffer->len] = '\0';
        }
    }

    return ESP_OK;
}

static bool parse_hex_byte(const char *text, uint8_t *out_value)
{
    char byte_text[3] = {
        text[0],
        text[1],
        '\0',
    };

    char *end = NULL;
    long value = strtol(byte_text, &end, 16);

    if (end == byte_text || *end != '\0' || value < 0 || value > 0xFF) {
        return false;
    }

    *out_value = (uint8_t)value;
    return true;
}

static bool parse_mac_address(const char *text, uint8_t mac[ESP_NOW_ETH_ALEN])
{
    if (text == NULL) {
        return false;
    }

    while (*text == ' ' || *text == '\r' || *text == '\n' || *text == '\t') {
        text++;
    }

    size_t len = strlen(text);
    while (len > 0 &&
           (text[len - 1] == ' ' ||
            text[len - 1] == '\r' ||
            text[len - 1] == '\n' ||
            text[len - 1] == '\t')) {
        len--;
    }

    if (len == 17) {
        for (int i = 0; i < ESP_NOW_ETH_ALEN; i++) {
            if (!parse_hex_byte(text + (i * 3), &mac[i])) {
                return false;
            }

            if (i < ESP_NOW_ETH_ALEN - 1 && text[(i * 3) + 2] != ':') {
                return false;
            }
        }

        return true;
    }

    if (len == 12) {
        for (int i = 0; i < ESP_NOW_ETH_ALEN; i++) {
            if (!parse_hex_byte(text + (i * 2), &mac[i])) {
                return false;
            }
        }

        return true;
    }

    return false;
}

static bool parse_light_state_response(const char *text, uint32_t *out_state)
{
    if (text == NULL || out_state == NULL) {
        return false;
    }

    while (*text == ' ' || *text == '\r' || *text == '\n' || *text == '\t') {
        text++;
    }

    if (strcmp(text, "0") == 0 || strcmp(text, "1") == 0) {
        *out_state = (uint32_t)atoi(text);
        return true;
    }

    size_t prefix_len = strlen(ESP_NOW_LIGHT_STATE_PREFIX);
    if (strncmp(text, ESP_NOW_LIGHT_STATE_PREFIX, prefix_len) == 0 &&
        (text[prefix_len] == '0' || text[prefix_len] == '1')) {
        char next = text[prefix_len + 1];
        if (next == '\0' ||
            next == ' ' ||
            next == '\r' ||
            next == '\n' ||
            next == '\t') {
            *out_state = text[prefix_len] == '1' ? 1 : 0;
            return true;
        }
    }

    if ((text[0] == '0' || text[0] == '1') &&
        (text[1] == '\0' ||
         text[1] == ' ' ||
         text[1] == '\r' ||
         text[1] == '\n' ||
         text[1] == '\t')) {
        *out_state = text[0] == '1' ? 1 : 0;
        return true;
    }

    return false;
}

static void esp_now_recv_cb(const esp_now_recv_info_t *recv_info,
                            const uint8_t *data,
                            int len)
{
    if (recv_info == NULL ||
        data == NULL ||
        len <= 0 ||
        !server_mac_known ||
        memcmp(recv_info->src_addr, server_mac, ESP_NOW_ETH_ALEN) != 0) {
        return;
    }

    char message[32] = {0};
    size_t copy_len = len;

    if (copy_len >= sizeof(message)) {
        copy_len = sizeof(message) - 1;
    }

    memcpy(message, data, copy_len);

    uint32_t new_state;
    if (parse_light_state_response(message, &new_state)) {
        esp_now_reply_state = new_state;
        xEventGroupSetBits(esp_now_event_group, ESP_NOW_RESPONSE_BIT);
    } else {
        ESP_LOGW(TAG, "Unexpected ESP-NOW response: '%s'", message);
    }
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
    gpio_deep_sleep_hold_dis();

    gpio_set_direction(ONBOARD_LED_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(ONBOARD_LED_GPIO, light_state);
    gpio_hold_dis(ONBOARD_LED_GPIO);

    gpio_set_direction(EXTERNAL_LIGHTS_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(EXTERNAL_LIGHTS_GPIO, !light_state);
    gpio_hold_dis(EXTERNAL_LIGHTS_GPIO);
}

static void apply_light_state(void)
{
    gpio_set_level(ONBOARD_LED_GPIO, light_state);
    gpio_set_level(EXTERNAL_LIGHTS_GPIO, !light_state);
}

static void enter_deep_sleep(void)
{
    apply_light_state();

    ESP_ERROR_CHECK(gpio_hold_en(ONBOARD_LED_GPIO));
    ESP_ERROR_CHECK(gpio_hold_en(EXTERNAL_LIGHTS_GPIO));
    gpio_deep_sleep_hold_en();

    ESP_ERROR_CHECK(esp_sleep_enable_timer_wakeup(REQUEST_INTERVAL_US));

    ESP_LOGI(TAG, "Sleeping for %llu us", REQUEST_INTERVAL_US);
    esp_deep_sleep_start();
}

static bool fetch_http_response(const char *url, char *buffer, size_t size)
{
    if (url == NULL || buffer == NULL || size == 0) {
        return false;
    }

    buffer[0] = '\0';

    http_response_buffer_t response = {
        .data = buffer,
        .size = size,
        .len = 0,
    };

    esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = 3000,
        .event_handler = http_event_handler,
        .user_data = &response,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);

    if (client == NULL) {
        return false;
    }

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);

    ESP_LOGI(TAG,
             "HTTP GET %s err=%s status=%d body='%s'",
             url,
             esp_err_to_name(err),
             status,
             buffer);

    esp_http_client_cleanup(client);

    return err == ESP_OK && status == 200 && buffer[0] != '\0';
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

    if (!fetch_http_response(SERVER_URL, buffer, sizeof(buffer))) {
        return false;
    }

    return parse_light_state_response(buffer, out_state);
}

static bool fetch_server_mac(uint8_t mac[ESP_NOW_ETH_ALEN])
{
    char buffer[32] = {0};

    if (!fetch_http_response(SERVER_MAC_URL, buffer, sizeof(buffer))) {
        return false;
    }

    if (!parse_mac_address(buffer, mac)) {
        ESP_LOGW(TAG, "Invalid MAC response from %s: '%s'",
                 SERVER_MAC_URL,
                 buffer);
        return false;
    }

    ESP_LOGI(TAG,
             "Server MAC: %02X:%02X:%02X:%02X:%02X:%02X",
             mac[0],
             mac[1],
             mac[2],
             mac[3],
             mac[4],
             mac[5]);

    return true;
}

static bool init_esp_now_peer(const uint8_t mac[ESP_NOW_ETH_ALEN])
{
    if (!esp_now_ready) {
        esp_err_t err = esp_now_init();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_now_init failed: %s", esp_err_to_name(err));
            return false;
        }

        err = esp_now_register_recv_cb(esp_now_recv_cb);
        if (err != ESP_OK) {
            ESP_LOGE(TAG,
                     "esp_now_register_recv_cb failed: %s",
                     esp_err_to_name(err));
            esp_now_deinit();
            return false;
        }

        esp_now_event_group = xEventGroupCreate();
        esp_now_ready = true;
    }

    if (esp_now_is_peer_exist(mac)) {
        return true;
    }

    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, mac, ESP_NOW_ETH_ALEN);
    peer.ifidx = WIFI_IF_STA;
    peer.channel = 0;
    peer.encrypt = false;

    esp_err_t err = esp_now_add_peer(&peer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_now_add_peer failed: %s", esp_err_to_name(err));
        return false;
    }

    return true;
}

static bool bootstrap_server(void)
{
    uint32_t initial_state;
    uint8_t mac[ESP_NOW_ETH_ALEN];

    if (!fetch_light_state(&initial_state)) {
        return false;
    }

    light_state = initial_state;
    apply_light_state();
    save_light_state(light_state);

    if (!fetch_server_mac(mac)) {
        return false;
    }

    if (!init_esp_now_peer(mac)) {
        return false;
    }

    memcpy(server_mac, mac, ESP_NOW_ETH_ALEN);
    server_mac_known = true;
    save_server_mac(server_mac);

    return true;
}

static bool refresh_server_mac(void)
{
    uint8_t mac[ESP_NOW_ETH_ALEN];

    if (!fetch_server_mac(mac)) {
        return false;
    }

    if (!init_esp_now_peer(mac)) {
        return false;
    }

    memcpy(server_mac, mac, ESP_NOW_ETH_ALEN);
    server_mac_known = true;
    save_server_mac(server_mac);

    return true;
}

static bool fetch_light_state_esp_now_once(uint32_t *out_state)
{
    if (!esp_now_ready || !server_mac_known || out_state == NULL) {
        return false;
    }

    xEventGroupClearBits(esp_now_event_group, ESP_NOW_RESPONSE_BIT);

    esp_err_t err = esp_now_send(
        server_mac,
        (const uint8_t *)ESP_NOW_GET_LIGHT_STATE,
        strlen(ESP_NOW_GET_LIGHT_STATE)
    );

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_now_send failed: %s", esp_err_to_name(err));
        return false;
    }

    EventBits_t bits = xEventGroupWaitBits(
        esp_now_event_group,
        ESP_NOW_RESPONSE_BIT,
        pdTRUE,
        pdFALSE,
        pdMS_TO_TICKS(1000)
    );

    if ((bits & ESP_NOW_RESPONSE_BIT) == 0) {
        ESP_LOGW(TAG, "Timed out waiting for ESP-NOW light state");
        return false;
    }

    *out_state = esp_now_reply_state;
    return true;
}

static bool fetch_light_state_esp_now(uint32_t *out_state)
{
    for (int attempt = 1; attempt <= ESP_NOW_MAX_RETRIES; attempt++) {
        if (fetch_light_state_esp_now_once(out_state)) {
            return true;
        }

        ESP_LOGW(TAG,
                 "ESP-NOW light state attempt %d/%d failed",
                 attempt,
                 ESP_NOW_MAX_RETRIES);
    }

    return false;
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

    load_persistent_state();

    // Initialize LED GPIO.
    init_gpio();

    // Initialize and start Wi-Fi station mode.
    init_wifi_sta();

    EventBits_t bits = xEventGroupWaitBits(
        wifi_event_group,
        WIFI_CONNECTED_BIT,
        pdFALSE,
        pdFALSE,
        pdMS_TO_TICKS(WIFI_CONNECT_TIMEOUT_MS)
    );

    if ((bits & WIFI_CONNECTED_BIT) == 0) {
        ESP_LOGW(TAG, "Wi-Fi connect timed out, sleeping");
        enter_deep_sleep();
    }

    if (!server_mac_known) {
        if (!bootstrap_server()) {
            ESP_LOGW(TAG, "Server bootstrap failed");
        }
        enter_deep_sleep();
    }

    uint32_t new_state;
    if (!init_esp_now_peer(server_mac)) {
        ESP_LOGW(TAG, "ESP-NOW peer setup failed, refreshing server MAC");
        refresh_server_mac();
        enter_deep_sleep();
    }

    if (fetch_light_state_esp_now(&new_state)) {
        bool should_save_state =
            !light_state_persisted || new_state != light_state;

        light_state = new_state;
        apply_light_state();

        if (should_save_state) {
            save_light_state(light_state);
        }
    } else {
        ESP_LOGW(TAG,
                 "ESP-NOW failed after %d attempts, refreshing server MAC",
                 ESP_NOW_MAX_RETRIES);

        if (!refresh_server_mac()) {
            server_mac_known = false;
        }
    }

    enter_deep_sleep();
}
