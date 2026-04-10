#include <string.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_mac.h>
#include <esp_event.h>
#include <esp_log.h>
#include <esp_netif.h>
#include "config.h"
#include "wifi_manager.h"
#include "nvs_keys.h"


static const char *TAG = "WIFI_MNG";
static bool sta_connected = false;
static esp_netif_t *sta_netif = NULL;
static int sta_retry_count = 0;
static bool sta_reconnect_enabled = true;
#define STA_MAX_RETRY 5


static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
        case WIFI_EVENT_AP_STACONNECTED: {
            wifi_event_ap_staconnected_t* e = (wifi_event_ap_staconnected_t*) event_data;
            ESP_LOGI(TAG, "AP: station " MACSTR " join, AID=%d", MAC2STR(e->mac), e->aid);
            break;
        }
        case WIFI_EVENT_AP_STADISCONNECTED: {
            wifi_event_ap_stadisconnected_t* e = (wifi_event_ap_stadisconnected_t*) event_data;
            ESP_LOGI(TAG, "AP: station " MACSTR " leave, AID=%d", MAC2STR(e->mac), e->aid);
            break;
        }
        case WIFI_EVENT_STA_DISCONNECTED:
            sta_connected = false;
            if (sta_reconnect_enabled && sta_retry_count < STA_MAX_RETRY) {
                sta_retry_count++;
                ESP_LOGI(TAG, "STA: disconnected, retry %d/%d", sta_retry_count, STA_MAX_RETRY);
                esp_wifi_connect();
            } else if (!sta_reconnect_enabled) {
                ESP_LOGI(TAG, "STA: disconnected (reconnect disabled)");
            } else {
                ESP_LOGW(TAG, "STA: connection failed after %d retries", STA_MAX_RETRY);
            }
            break;
        case WIFI_EVENT_STA_CONNECTED:
            ESP_LOGI(TAG, "STA: connected to AP");
            sta_retry_count = 0;
            break;
        default:
            break;
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* e = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "STA: got IP " IPSTR, IP2STR(&e->ip_info.ip));
        sta_connected = true;
    }
}


esp_err_t wifi_init(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    esp_netif_t *ap_netif = esp_netif_create_default_wifi_ap();
    if (ap_netif == NULL) {
        ESP_LOGE(TAG, "Failed to create AP netif");
        return ESP_FAIL;
    }

    sta_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    /* Check if STA credentials are stored */
    char sta_ssid[33] = {0};
    bool has_sta = (read_string_from_flash(STA_SSID_KEY, sta_ssid, sizeof(sta_ssid)) == ESP_OK && sta_ssid[0]);

    if (has_sta) {
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
        ESP_LOGI(TAG, "WiFi mode: APSTA");
    } else {
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
        ESP_LOGI(TAG, "WiFi mode: AP only (no STA credentials)");
    }

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    return ESP_OK;
}


void wifi_start(void)
{
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_LOGI(TAG, "WiFi started");
}


void wifi_start_softap(void)
{
    wifi_config_t wifi_config = {
        .ap = {
            .channel = 1,
            .authmode = WIFI_AUTH_WPA_WPA2_PSK,
            .max_connection = 5,
            .pmf_cfg = { .required = false, .capable = false }
        }
    };
    strlcpy((char *)wifi_config.ap.ssid, config_default_ap_ssid(), sizeof(wifi_config.ap.ssid));
    strlcpy((char *)wifi_config.ap.password, config_default_ap_pass(), sizeof(wifi_config.ap.password));
    wifi_config.ap.channel = config_default_ap_channel();

    read_string_from_flash(WIFI_SSID_KEY, (char *)wifi_config.ap.ssid, sizeof(wifi_config.ap.ssid));
    wifi_config.ap.ssid_len = strlen((char *)wifi_config.ap.ssid);

    /* Read password from NVS, but fall back to default if empty/short
     * (ESP-IDF silently downgrades to OPEN if password < 8 chars) */
    char nvs_pass[64] = {0};
    if (read_string_from_flash(WIFI_PASS_KEY, nvs_pass, sizeof(nvs_pass)) == ESP_OK
        && strlen(nvs_pass) >= 8) {
        strlcpy((char *)wifi_config.ap.password, nvs_pass, sizeof(wifi_config.ap.password));
    }

    /* Read channel into temp var — channel is uint8_t, NVS stores int32_t.
     * Writing int32_t directly to uint8_t* overflows into authmode! */
    int32_t ch = wifi_config.ap.channel;
    read_int_from_flash(WIFI_CHAN_KEY, &ch);
    wifi_config.ap.channel = (uint8_t)ch;

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_LOGI(TAG, "AP started: SSID=%s CH=%d", wifi_config.ap.ssid, wifi_config.ap.channel);
}


void wifi_start_sta(void)
{
    char sta_ssid[33] = {0};
    char sta_pass[65] = {0};

    if (read_string_from_flash(STA_SSID_KEY, sta_ssid, sizeof(sta_ssid)) != ESP_OK || !sta_ssid[0]) {
        ESP_LOGI(TAG, "STA: no credentials configured, skipping");
        return;
    }
    read_string_from_flash(STA_PASS_KEY, sta_pass, sizeof(sta_pass));

    wifi_config_t sta_config = {0};
    strlcpy((char *)sta_config.sta.ssid, sta_ssid, sizeof(sta_config.sta.ssid));
    strlcpy((char *)sta_config.sta.password, sta_pass, sizeof(sta_config.sta.password));

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_config));
    ESP_LOGI(TAG, "STA: configured for '%s'", sta_ssid);
}


void wifi_connect_sta(void)
{
    char sta_ssid[33] = {0};
    if (read_string_from_flash(STA_SSID_KEY, sta_ssid, sizeof(sta_ssid)) != ESP_OK || !sta_ssid[0]) {
        return;
    }
    ESP_LOGI(TAG, "STA: connecting to '%s'...", sta_ssid);
    sta_retry_count = 0;
    esp_wifi_connect();
}


bool wifi_sta_is_connected(void)
{
    return sta_connected;
}


void wifi_sta_set_reconnect(bool enable)
{
    sta_reconnect_enabled = enable;
    if (!enable) sta_retry_count = STA_MAX_RETRY; /* stop retries immediately */
}


esp_err_t wifi_get_sta_ip(char *buf, int buf_len)
{
    if (!sta_connected || !sta_netif) return ESP_FAIL;

    esp_netif_ip_info_t ip_info;
    esp_err_t err = esp_netif_get_ip_info(sta_netif, &ip_info);
    if (err != ESP_OK) return err;

    snprintf(buf, buf_len, IPSTR, IP2STR(&ip_info.ip));
    return ESP_OK;
}
