#include <string.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_log.h>
#include <esp_wifi.h>
#include <esp_wps.h>
#include <cJSON.h>
#include "wifi_wps.h"
#include "wifi_manager.h"
#include "wifi_saved.h"
#include "server.h"

static const char *TAG = "WPS";

/* WPS state */
static wps_status_t wps_status;
static TaskHandle_t wps_timer_task_handle = NULL;
static bool wps_running = false;

/* Saved STA config for restore after WPS */
static wifi_config_t saved_sta_config;
static bool had_sta_config = false;


/* ---- WebSocket push helpers ---- */

static void wps_push_status(const char *phase, const char *message)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) return;

    cJSON_AddStringToObject(root, "type", "ws");
    cJSON_AddStringToObject(root, "phase", phase);
    cJSON_AddStringToObject(root, "message", message);
    cJSON_AddNumberToObject(root, "elapsed", wps_status.elapsed_sec);
    cJSON_AddNumberToObject(root, "timeout", wps_status.timeout_sec);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) return;

    ws_frame_req_t cmd = {
        .hd = get_web_server_handle(),
        .fd = -1,
        .payload = json, .len = strlen(json), .need_free = true
    };
    if (ws_send_broadcast_to_queue(&cmd) != ESP_OK) free(json);
}


static void wps_push_result(void)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) return;

    char bssid_str[18];
    snprintf(bssid_str, sizeof(bssid_str), "%02X:%02X:%02X:%02X:%02X:%02X",
             wps_status.captured_bssid[0], wps_status.captured_bssid[1],
             wps_status.captured_bssid[2], wps_status.captured_bssid[3],
             wps_status.captured_bssid[4], wps_status.captured_bssid[5]);

    cJSON_AddStringToObject(root, "type", "wr");
    cJSON_AddStringToObject(root, "ssid", wps_status.captured_ssid);
    cJSON_AddStringToObject(root, "password", wps_status.captured_pass);
    cJSON_AddStringToObject(root, "bssid", bssid_str);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) return;

    ws_frame_req_t cmd = {
        .hd = get_web_server_handle(),
        .fd = -1,
        .payload = json, .len = strlen(json), .need_free = true
    };
    if (ws_send_broadcast_to_queue(&cmd) != ESP_OK) free(json);
}


static void wps_push_error(const char *message)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) return;

    cJSON_AddStringToObject(root, "type", "we");
    cJSON_AddStringToObject(root, "message", message);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) return;

    ws_frame_req_t cmd = {
        .hd = get_web_server_handle(),
        .fd = -1,
        .payload = json, .len = strlen(json), .need_free = true
    };
    if (ws_send_broadcast_to_queue(&cmd) != ESP_OK) free(json);
}


/* ---- WPS event handler ---- */

static void wps_event_handler(void *arg, esp_event_base_t event_base,
                              int32_t event_id, void *event_data)
{
    if (event_base != WIFI_EVENT) return;

    switch (event_id) {
    case WIFI_EVENT_STA_WPS_ER_SUCCESS: {
        ESP_LOGI(TAG, "WPS PBC success!");
        wifi_event_sta_wps_er_success_t *evt =
            (wifi_event_sta_wps_er_success_t *)event_data;

        if (evt && evt->ap_cred_cnt > 0) {
            /* Store captured credentials */
            strlcpy(wps_status.captured_ssid,
                    (const char *)evt->ap_cred[0].ssid,
                    sizeof(wps_status.captured_ssid));
            strlcpy(wps_status.captured_pass,
                    (const char *)evt->ap_cred[0].passphrase,
                    sizeof(wps_status.captured_pass));

            ESP_LOGI(TAG, "Captured SSID: %s", wps_status.captured_ssid);
            ESP_LOGI(TAG, "Captured PASS: %s", wps_status.captured_pass);

            /* Connect to get the BSSID */
            wifi_config_t cfg = {0};
            strlcpy((char *)cfg.sta.ssid, wps_status.captured_ssid, sizeof(cfg.sta.ssid));
            strlcpy((char *)cfg.sta.password, wps_status.captured_pass, sizeof(cfg.sta.password));
            esp_wifi_set_config(WIFI_IF_STA, &cfg);

            esp_wifi_wps_disable();
            esp_wifi_connect();
        } else {
            /* Credentials not in event, try connecting directly */
            esp_wifi_wps_disable();
            esp_wifi_connect();
        }
        break;
    }

    case WIFI_EVENT_STA_CONNECTED: {
        if (wps_status.state != WPS_STATE_LISTENING) break;

        /* Get connected AP info for BSSID */
        wifi_ap_record_t ap_info;
        if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
            memcpy(wps_status.captured_bssid, ap_info.bssid, 6);

            /* Get SSID/password from WiFi config — the WPS stack has already
             * provisioned it (STA_CONNECTED fires before WPS_ER_SUCCESS,
             * so event credentials are not available yet). */
            wifi_config_t sta_cfg;
            if (esp_wifi_get_config(WIFI_IF_STA, &sta_cfg) == ESP_OK) {
                if (sta_cfg.sta.ssid[0]) {
                    strlcpy(wps_status.captured_ssid,
                            (const char *)sta_cfg.sta.ssid,
                            sizeof(wps_status.captured_ssid));
                }
                if (sta_cfg.sta.password[0]) {
                    strlcpy(wps_status.captured_pass,
                            (const char *)sta_cfg.sta.password,
                            sizeof(wps_status.captured_pass));
                }
            }

            /* Fallback: SSID from ap_info */
            if (wps_status.captured_ssid[0] == '\0') {
                strlcpy(wps_status.captured_ssid,
                        (const char *)ap_info.ssid,
                        sizeof(wps_status.captured_ssid));
            }

            /* Check BSSID filter */
            if (wps_status.has_target &&
                memcmp(wps_status.captured_bssid, wps_status.target_bssid, 6) != 0) {
                ESP_LOGW(TAG, "WPS success but wrong AP — restarting listener");
                wps_push_status("mismatch", "Connected to wrong AP, restarting...");
                esp_wifi_disconnect();

                /* Re-enable WPS and continue listening */
                esp_wps_config_t wps_cfg = WPS_CONFIG_INIT_DEFAULT(WPS_TYPE_PBC);
                esp_wifi_wps_enable(&wps_cfg);
                esp_wifi_wps_start(0);
                break;
            }

            wps_status.state = WPS_STATE_SUCCESS;
            wps_running = false;

            ESP_LOGI(TAG, "WPS captured: SSID=%s PASS=%s BSSID=%02X:%02X:%02X:%02X:%02X:%02X",
                     wps_status.captured_ssid, wps_status.captured_pass,
                     wps_status.captured_bssid[0], wps_status.captured_bssid[1],
                     wps_status.captured_bssid[2], wps_status.captured_bssid[3],
                     wps_status.captured_bssid[4], wps_status.captured_bssid[5]);

            wps_push_result();
            wps_push_status("success", "Credentials captured!");

            /* Auto-save to NVS */
            {
                char bssid_str[18];
                snprintf(bssid_str, sizeof(bssid_str),
                         "%02X:%02X:%02X:%02X:%02X:%02X",
                         wps_status.captured_bssid[0], wps_status.captured_bssid[1],
                         wps_status.captured_bssid[2], wps_status.captured_bssid[3],
                         wps_status.captured_bssid[4], wps_status.captured_bssid[5]);
                wifi_saved_add(wps_status.captured_ssid,
                               wps_status.captured_pass, bssid_str);
            }

            /* Disconnect — we got what we needed */
            esp_wifi_disconnect();
        }
        break;
    }

    case WIFI_EVENT_STA_WPS_ER_FAILED:
        ESP_LOGW(TAG, "WPS failed, retrying...");
        wps_push_status("retry", "WPS exchange failed, retrying...");
        esp_wifi_wps_disable();
        if (wps_running) {
            esp_wps_config_t wps_cfg = WPS_CONFIG_INIT_DEFAULT(WPS_TYPE_PBC);
            esp_wifi_wps_enable(&wps_cfg);
            esp_wifi_wps_start(0);
        }
        break;

    case WIFI_EVENT_STA_WPS_ER_TIMEOUT:
        ESP_LOGW(TAG, "WPS scan timeout, restarting...");
        if (wps_running) {
            wps_push_status("retry", "WPS scan cycle timeout, restarting...");
            esp_wifi_wps_disable();
            esp_wps_config_t wps_cfg = WPS_CONFIG_INIT_DEFAULT(WPS_TYPE_PBC);
            esp_wifi_wps_enable(&wps_cfg);
            esp_wifi_wps_start(0);
        }
        break;

    default:
        break;
    }
}


/* ---- Timer task: tracks elapsed time and enforces timeout ---- */

static void wps_timer_task(void *arg)
{
    while (wps_running) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        wps_status.elapsed_sec++;

        /* Push periodic status: first at 1s, then every 10s */
        if (wps_status.elapsed_sec == 1 || wps_status.elapsed_sec % 10 == 0) {
            char msg[64];
            snprintf(msg, sizeof(msg), "Listening... %ds / %ds",
                     wps_status.elapsed_sec, wps_status.timeout_sec);
            wps_push_status("listening", msg);
        }

        /* Check timeout */
        if (wps_status.timeout_sec > 0 &&
            wps_status.elapsed_sec >= wps_status.timeout_sec) {
            ESP_LOGI(TAG, "WPS listener timeout reached (%ds)", wps_status.timeout_sec);
            wps_status.state = WPS_STATE_TIMEOUT;
            wps_running = false;

            esp_wifi_wps_disable();
            wps_push_status("timeout", "WPS listener timeout — no button pressed");

            /* Restore original STA if any */
            if (had_sta_config && saved_sta_config.sta.ssid[0]) {
                esp_wifi_set_config(WIFI_IF_STA, &saved_sta_config);
                esp_wifi_connect();
            }
            break;
        }
    }

    wps_timer_task_handle = NULL;
    vTaskDelete(NULL);
}


/* ---- Public API ---- */

/* Parameters passed to the WPS start task */
static uint8_t  wps_start_bssid[6];
static bool     wps_start_has_bssid;
static int      wps_start_timeout;

static void wps_start_task(void *arg)
{
    /* Let the API response reach the client before touching WiFi.
     * On dual-core ESP32 this task may start immediately on the other core;
     * WiFi operations (disconnect / mode switch) can briefly block the
     * radio, preventing the httpd from flushing the WebSocket reply. */
    vTaskDelay(pdMS_TO_TICKS(500));

    /* Save current STA config */
    had_sta_config = (esp_wifi_get_config(WIFI_IF_STA, &saved_sta_config) == ESP_OK
                      && saved_sta_config.sta.ssid[0]);

    /* Disable STA auto-reconnect and disconnect */
    wifi_sta_set_reconnect(false);
    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(300));

    /* Ensure APSTA mode */
    wifi_mode_t mode;
    esp_wifi_get_mode(&mode);
    if (mode == WIFI_MODE_AP) {
        esp_wifi_set_mode(WIFI_MODE_APSTA);
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    /* Reset status */
    memset(&wps_status, 0, sizeof(wps_status));
    wps_status.state = WPS_STATE_LISTENING;
    wps_status.timeout_sec = wps_start_timeout;

    if (wps_start_has_bssid) {
        memcpy(wps_status.target_bssid, wps_start_bssid, 6);
        wps_status.has_target = true;
    }

    /* Register WPS event handler */
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wps_event_handler, NULL);

    /* Enable and start WPS PBC */
    esp_wps_config_t wps_cfg = WPS_CONFIG_INIT_DEFAULT(WPS_TYPE_PBC);
    esp_err_t err = esp_wifi_wps_enable(&wps_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WPS enable failed: %s", esp_err_to_name(err));
        wps_status.state = WPS_STATE_FAILED;
        wps_push_error("WPS enable failed");
        wifi_sta_set_reconnect(true);
        vTaskDelete(NULL);
        return;
    }

    err = esp_wifi_wps_start(0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WPS start failed: %s", esp_err_to_name(err));
        esp_wifi_wps_disable();
        wps_status.state = WPS_STATE_FAILED;
        wps_push_error("WPS start failed");
        wifi_sta_set_reconnect(true);
        vTaskDelete(NULL);
        return;
    }

    wps_running = true;

    if (wps_status.has_target) {
        ESP_LOGI(TAG, "WPS PBC listener started — target %02X:%02X:%02X:%02X:%02X:%02X, timeout %ds",
                 wps_status.target_bssid[0], wps_status.target_bssid[1],
                 wps_status.target_bssid[2], wps_status.target_bssid[3],
                 wps_status.target_bssid[4], wps_status.target_bssid[5],
                 wps_start_timeout);
    } else {
        ESP_LOGI(TAG, "WPS PBC listener started — any AP, timeout %ds", wps_start_timeout);
    }

    wps_push_status("started", "WPS PBC listener active");

    /* Start timer task */
    xTaskCreate(wps_timer_task, "wps_timer", 2048, NULL, 5, &wps_timer_task_handle);

    vTaskDelete(NULL);
}


esp_err_t wifi_wps_listen_start(const uint8_t *target_bssid, int timeout_sec)
{
    if (wps_running) {
        ESP_LOGW(TAG, "WPS already running");
        return ESP_ERR_INVALID_STATE;
    }

    /* Store params for the task */
    wps_start_has_bssid = false;
    memset(wps_start_bssid, 0, 6);
    if (target_bssid) {
        bool all_zero = true;
        for (int i = 0; i < 6; i++) {
            if (target_bssid[i] != 0) { all_zero = false; break; }
        }
        if (!all_zero) {
            memcpy(wps_start_bssid, target_bssid, 6);
            wps_start_has_bssid = true;
        }
    }
    wps_start_timeout = timeout_sec;

    /* Launch in background task so API responds immediately */
    xTaskCreate(wps_start_task, "wps_start", 4096, NULL, 5, NULL);
    return ESP_OK;
}


void wifi_wps_listen_stop(void)
{
    if (!wps_running && wps_status.state == WPS_STATE_IDLE) return;

    wps_running = false;

    esp_wifi_wps_disable();
    esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, &wps_event_handler);

    if (wps_status.state == WPS_STATE_LISTENING) {
        wps_status.state = WPS_STATE_IDLE;
    }

    /* Wait for timer task to finish */
    if (wps_timer_task_handle) {
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    /* Re-enable STA reconnect and restore original STA */
    wifi_sta_set_reconnect(true);
    if (had_sta_config && saved_sta_config.sta.ssid[0]) {
        ESP_LOGI(TAG, "Restoring STA: %s", saved_sta_config.sta.ssid);
        esp_wifi_set_config(WIFI_IF_STA, &saved_sta_config);
        esp_wifi_connect();
    }

    wps_push_status("stopped", "WPS listener stopped");
    ESP_LOGI(TAG, "WPS listener stopped");
}


const wps_status_t *wifi_wps_get_status(void)
{
    return &wps_status;
}
