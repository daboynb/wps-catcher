#include <string.h>
#include <esp_timer.h>
#include <esp_log.h>
#include <cJSON.h>
#include "config.h"
#include "server_api.h"
#include "nvs_keys.h"
#include "wifi_manager.h"
#include "wifi_wps.h"
#include "wifi_saved.h"


static const char *TAG = "API";

typedef struct {
    api_command_t cmd;
    esp_err_t (*handler)(ws_frame_req_t *req);
} api_cmd_t;


const char* mime_from_path(const char* path)
{
    const char *ext = strrchr(path, '.');
    if (!ext) return "text/plain";
    if (strcmp(ext, ".html") == 0) return "text/html";
    if (strcmp(ext, ".css")  == 0) return "text/css";
    if (strcmp(ext, ".js")   == 0) return "application/javascript";
    if (strcmp(ext, ".png")  == 0) return "image/png";
    if (strcmp(ext, ".ico")  == 0) return "image/x-icon";
    if (strcmp(ext, ".svg")  == 0) return "image/svg+xml";
    return "text/plain";
}


static void api_send_response(ws_frame_req_t *req, const char* status, const char *message)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) return;

    cJSON_AddNumberToObject(root, "req_id", req->req_id);
    cJSON_AddStringToObject(root, "status", status);
    cJSON_AddStringToObject(root, "message", message);

    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!payload) return;

    ws_frame_req_t cmd = {
        .hd = req->hd,
        .fd = req->fd,
        .payload = payload,
        .len = strlen(payload),
        .need_free = true
    };

    if (ws_send_command_to_queue(&cmd) != ESP_OK) {
        free(payload);
    }
}


/* GET_STATUS: uptime, RAM usage */
static esp_err_t api_get_status(ws_frame_req_t *req)
{
    int time_s = (int)(esp_timer_get_time() / 1000000);
    int hours = time_s / 3600;
    int minutes = (time_s % 3600) / 60;
    int seconds = time_s % 60;
    char uptime[16];
    snprintf(uptime, sizeof(uptime), "%02d:%02d:%02d", hours, minutes, seconds);

    size_t total_ram = heap_caps_get_total_size(MALLOC_CAP_DEFAULT);
    size_t free_ram = heap_caps_get_free_size(MALLOC_CAP_DEFAULT);
    int ram_pct = (total_ram > 0) ? 100 - ((free_ram * 100) / total_ram) : 0;

    cJSON *root = cJSON_CreateObject();
    if (!root) return ESP_FAIL;

    cJSON_AddNumberToObject(root, "req_id", req->req_id);
    cJSON_AddStringToObject(root, "type", "get_status");
    cJSON_AddStringToObject(root, "uptime", uptime);
    cJSON_AddNumberToObject(root, "ram", ram_pct);
    cJSON_AddNumberToObject(root, "free_heap", (int)free_ram);

    char sta_ip[16] = {0};
    if (wifi_get_sta_ip(sta_ip, sizeof(sta_ip)) == ESP_OK) {
        cJSON_AddStringToObject(root, "sta_ip", sta_ip);
    }
    cJSON_AddBoolToObject(root, "sta_connected", wifi_sta_is_connected());

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) return ESP_FAIL;

    ws_frame_req_t cmd = {
        .hd = req->hd, .fd = req->fd,
        .payload = json, .len = strlen(json), .need_free = true
    };
    if (ws_send_command_to_queue(&cmd) != ESP_OK) {
        free(json);
        return ESP_FAIL;
    }
    return ESP_OK;
}


/* GET_AP_SETTINGS */
static esp_err_t api_get_ap_settings(ws_frame_req_t *req)
{
    char ssid[32] = {0};
    char password[64] = {0};
    int32_t channel = config_default_ap_channel();

    if (read_string_from_flash(WIFI_SSID_KEY, ssid, sizeof(ssid)) != ESP_OK)
        strlcpy(ssid, config_default_ap_ssid(), sizeof(ssid));
    if (read_string_from_flash(WIFI_PASS_KEY, password, sizeof(password)) != ESP_OK)
        strlcpy(password, config_default_ap_pass(), sizeof(password));
    read_int_from_flash(WIFI_CHAN_KEY, &channel);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "req_id", req->req_id);
    cJSON_AddStringToObject(root, "type", "get_ap_settings");
    cJSON_AddStringToObject(root, "ssid", ssid);
    cJSON_AddStringToObject(root, "password", password);
    cJSON_AddNumberToObject(root, "channel", channel);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) return ESP_FAIL;

    ws_frame_req_t cmd = {
        .hd = req->hd, .fd = req->fd,
        .payload = json, .len = strlen(json), .need_free = true
    };
    if (ws_send_command_to_queue(&cmd) != ESP_OK) {
        free(json);
        return ESP_FAIL;
    }
    return ESP_OK;
}


/* SET_AP_SETTINGS */
static esp_err_t api_set_ap_settings(ws_frame_req_t *req)
{
    cJSON *json = cJSON_Parse(req->payload);
    if (!json) return ESP_FAIL;

    const cJSON *j_ssid = cJSON_GetObjectItemCaseSensitive(json, "ssid");
    const cJSON *j_pass = cJSON_GetObjectItemCaseSensitive(json, "password");
    const cJSON *j_chan = cJSON_GetObjectItemCaseSensitive(json, "channel");

    char ssid[32] = {0};
    char password[64] = {0};
    int channel = config_default_ap_channel();

    if (cJSON_IsString(j_ssid)) strlcpy(ssid, j_ssid->valuestring, sizeof(ssid));
    if (cJSON_IsString(j_pass)) strlcpy(password, j_pass->valuestring, sizeof(password));
    if (cJSON_IsNumber(j_chan)) channel = j_chan->valueint;
    cJSON_Delete(json);

    if (strlen(password) < 8) {
        api_send_response(req, "error", "Password must be at least 8 characters");
        return ESP_FAIL;
    }

    save_string_to_flash(WIFI_SSID_KEY, ssid);
    save_string_to_flash(WIFI_PASS_KEY, password);
    save_int_to_flash(WIFI_CHAN_KEY, channel);

    api_send_response(req, "ok", "AP settings saved. Reboot to apply.");
    return ESP_OK;
}


/* REBOOT */
static esp_err_t api_reboot(ws_frame_req_t *req)
{
    api_send_response(req, "ok", "Rebooting...");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}


/* GET_STA_SETTINGS */
static esp_err_t api_get_sta_settings(ws_frame_req_t *req)
{
    char ssid[33] = {0};
    char password[65] = {0};

    read_string_from_flash(STA_SSID_KEY, ssid, sizeof(ssid));
    read_string_from_flash(STA_PASS_KEY, password, sizeof(password));

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "req_id", req->req_id);
    cJSON_AddStringToObject(root, "type", "get_sta_settings");
    cJSON_AddStringToObject(root, "ssid", ssid);
    cJSON_AddStringToObject(root, "password", password);
    cJSON_AddBoolToObject(root, "connected", wifi_sta_is_connected());

    char sta_ip[16] = {0};
    if (wifi_get_sta_ip(sta_ip, sizeof(sta_ip)) == ESP_OK) {
        cJSON_AddStringToObject(root, "ip", sta_ip);
    }

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) return ESP_FAIL;

    ws_frame_req_t cmd = {
        .hd = req->hd, .fd = req->fd,
        .payload = json, .len = strlen(json), .need_free = true
    };
    if (ws_send_command_to_queue(&cmd) != ESP_OK) {
        free(json);
        return ESP_FAIL;
    }
    return ESP_OK;
}


/* SET_STA_SETTINGS */
static esp_err_t api_set_sta_settings(ws_frame_req_t *req)
{
    cJSON *json = cJSON_Parse(req->payload);
    if (!json) return ESP_FAIL;

    const cJSON *j_ssid = cJSON_GetObjectItemCaseSensitive(json, "ssid");
    const cJSON *j_pass = cJSON_GetObjectItemCaseSensitive(json, "password");

    char ssid[33] = {0};
    char password[65] = {0};

    if (cJSON_IsString(j_ssid)) strlcpy(ssid, j_ssid->valuestring, sizeof(ssid));
    if (cJSON_IsString(j_pass)) strlcpy(password, j_pass->valuestring, sizeof(password));
    cJSON_Delete(json);

    if (ssid[0] && password[0] && strlen(password) < 8) {
        api_send_response(req, "error", "Password must be at least 8 characters");
        return ESP_FAIL;
    }

    save_string_to_flash(STA_SSID_KEY, ssid);
    save_string_to_flash(STA_PASS_KEY, password);

    if (ssid[0]) {
        api_send_response(req, "ok", "STA settings saved. Reboot to connect.");
    } else {
        api_send_response(req, "ok", "STA disabled. Reboot to apply.");
    }
    return ESP_OK;
}


static esp_err_t api_wps_start(ws_frame_req_t *req)
{
    cJSON *json = cJSON_Parse(req->payload);
    if (!json) return ESP_FAIL;

    const cJSON *j_bssid = cJSON_GetObjectItemCaseSensitive(json, "bssid");
    const cJSON *j_timeout = cJSON_GetObjectItemCaseSensitive(json, "timeout");

    uint8_t target_bssid[6] = {0};
    bool has_target = false;

    if (cJSON_IsString(j_bssid) && strlen(j_bssid->valuestring) == 17) {
        unsigned b[6];
        sscanf(j_bssid->valuestring, "%x:%x:%x:%x:%x:%x",
               &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]);
        for (int i = 0; i < 6; i++) target_bssid[i] = (uint8_t)b[i];
        has_target = true;
    }

    int timeout_sec = cJSON_IsNumber(j_timeout) ? j_timeout->valueint : 3600;
    cJSON_Delete(json);

    esp_err_t err = wifi_wps_listen_start(has_target ? target_bssid : NULL, timeout_sec);
    if (err != ESP_OK) {
        api_send_response(req, "error", "WPS listener failed to start");
        return ESP_FAIL;
    }

    api_send_response(req, "ok", "WPS listener started");
    return ESP_OK;
}


static esp_err_t api_wps_stop(ws_frame_req_t *req)
{
    wifi_wps_listen_stop();
    api_send_response(req, "ok", "WPS listener stopped");
    return ESP_OK;
}


static esp_err_t api_wps_status(ws_frame_req_t *req)
{
    const wps_status_t *st = wifi_wps_get_status();

    cJSON *root = cJSON_CreateObject();
    if (!root) return ESP_FAIL;

    cJSON_AddNumberToObject(root, "req_id", req->req_id);
    cJSON_AddStringToObject(root, "type", "wps_status");

    const char *state_str;
    switch (st->state) {
    case WPS_STATE_LISTENING: state_str = "listening"; break;
    case WPS_STATE_SUCCESS:   state_str = "success"; break;
    case WPS_STATE_FAILED:    state_str = "failed"; break;
    case WPS_STATE_TIMEOUT:   state_str = "timeout"; break;
    default:                  state_str = "idle"; break;
    }
    cJSON_AddStringToObject(root, "state", state_str);
    cJSON_AddNumberToObject(root, "elapsed", st->elapsed_sec);
    cJSON_AddNumberToObject(root, "timeout", st->timeout_sec);

    if (st->state == WPS_STATE_SUCCESS) {
        char bssid[18];
        snprintf(bssid, sizeof(bssid), "%02X:%02X:%02X:%02X:%02X:%02X",
                 st->captured_bssid[0], st->captured_bssid[1],
                 st->captured_bssid[2], st->captured_bssid[3],
                 st->captured_bssid[4], st->captured_bssid[5]);
        cJSON_AddStringToObject(root, "ssid", st->captured_ssid);
        cJSON_AddStringToObject(root, "password", st->captured_pass);
        cJSON_AddStringToObject(root, "bssid", bssid);
    }

    char *json_out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json_out) return ESP_FAIL;

    ws_frame_req_t cmd = { .hd = req->hd, .fd = req->fd,
        .payload = json_out, .len = strlen(json_out), .need_free = true };
    if (ws_send_command_to_queue(&cmd) != ESP_OK) { free(json_out); return ESP_FAIL; }
    return ESP_OK;
}


/* SAVED_WIFI_LIST */
static esp_err_t api_saved_wifi_list(ws_frame_req_t *req)
{
    saved_wifi_entry_t *entries = calloc(MAX_SAVED_WIFI, sizeof(saved_wifi_entry_t));
    if (!entries) {
        api_send_response(req, "error", "Out of memory");
        return ESP_FAIL;
    }
    int count = wifi_saved_list(entries, MAX_SAVED_WIFI);

    cJSON *root = cJSON_CreateObject();
    if (!root) { free(entries); return ESP_FAIL; }

    cJSON_AddNumberToObject(root, "req_id", req->req_id);
    cJSON_AddStringToObject(root, "type", "saved_wifi_list");
    cJSON_AddNumberToObject(root, "count", count);

    cJSON *arr = cJSON_AddArrayToObject(root, "entries");
    for (int i = 0; i < count; i++) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "ssid", entries[i].ssid);
        cJSON_AddStringToObject(e, "password", entries[i].password);
        cJSON_AddStringToObject(e, "bssid", entries[i].bssid);
        cJSON_AddItemToArray(arr, e);
    }

    free(entries);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) return ESP_FAIL;

    ws_frame_req_t cmd = { .hd = req->hd, .fd = req->fd,
        .payload = json, .len = strlen(json), .need_free = true };
    if (ws_send_command_to_queue(&cmd) != ESP_OK) { free(json); return ESP_FAIL; }
    return ESP_OK;
}


/* SAVED_WIFI_DELETE */
static esp_err_t api_saved_wifi_delete(ws_frame_req_t *req)
{
    cJSON *json = cJSON_Parse(req->payload);
    if (!json) return ESP_FAIL;

    /* Accept either "bssid" string (preferred) or "index" number */
    const cJSON *j_bssid = cJSON_GetObjectItemCaseSensitive(json, "bssid");
    const cJSON *j_idx = cJSON_GetObjectItemCaseSensitive(json, "index");

    esp_err_t err = ESP_ERR_INVALID_ARG;

    if (cJSON_IsString(j_bssid) && j_bssid->valuestring[0]) {
        /* Find by BSSID and delete */
        saved_wifi_entry_t *entries = calloc(MAX_SAVED_WIFI, sizeof(saved_wifi_entry_t));
        if (entries) {
            int count = wifi_saved_list(entries, MAX_SAVED_WIFI);
            for (int i = 0; i < count; i++) {
                if (strcasecmp(entries[i].bssid, j_bssid->valuestring) == 0) {
                    err = wifi_saved_delete(i);
                    break;
                }
            }
            free(entries);
        }
    } else if (cJSON_IsNumber(j_idx)) {
        err = wifi_saved_delete(j_idx->valueint);
    }

    cJSON_Delete(json);

    if (err != ESP_OK) {
        api_send_response(req, "error", "Entry not found");
        return ESP_FAIL;
    }

    api_send_response(req, "ok", "Entry deleted");
    return ESP_OK;
}


/* SET_WEB_PASS */
static esp_err_t api_set_web_pass(ws_frame_req_t *req)
{
    cJSON *json = cJSON_Parse(req->payload);
    if (!json) return ESP_FAIL;

    const cJSON *j_pass = cJSON_GetObjectItemCaseSensitive(json, "password");
    if (!cJSON_IsString(j_pass) || strlen(j_pass->valuestring) < 4) {
        cJSON_Delete(json);
        api_send_response(req, "error", "Password must be at least 4 characters");
        return ESP_FAIL;
    }

    save_string_to_flash(WEB_PASS_KEY, j_pass->valuestring);
    cJSON_Delete(json);

    api_send_response(req, "ok", "Web password changed. Login again to apply.");
    return ESP_OK;
}


static const api_cmd_t api_cmd_list[] = {
    { API_GET_STATUS,        api_get_status },
    { API_SET_AP_SETTINGS,   api_set_ap_settings },
    { API_GET_AP_SETTINGS,   api_get_ap_settings },
    { API_REBOOT,            api_reboot },
    { API_SET_STA_SETTINGS,  api_set_sta_settings },
    { API_GET_STA_SETTINGS,  api_get_sta_settings },
    { API_WPS_START,         api_wps_start },
    { API_WPS_STOP,          api_wps_stop },
    { API_WPS_STATUS,        api_wps_status },
    { API_SAVED_WIFI_LIST,   api_saved_wifi_list },
    { API_SAVED_WIFI_DELETE, api_saved_wifi_delete },
    { API_SET_WEB_PASS,      api_set_web_pass },
};


void http_api_parse(ws_frame_req_t *req)
{
    cJSON *root = cJSON_Parse(req->payload);
    if (!root) {
        ESP_LOGE(TAG, "Invalid JSON: %s", req->payload);
        return;
    }

    cJSON *jcmd = cJSON_GetObjectItemCaseSensitive(root, "cmd");
    if (!cJSON_IsNumber(jcmd)) {
        cJSON_Delete(root);
        return;
    }

    cJSON *jid = cJSON_GetObjectItemCaseSensitive(root, "req_id");
    req->req_id = cJSON_IsNumber(jid) ? jid->valueint : 0;

    int cmd = jcmd->valueint;
    bool handled = false;
    for (size_t i = 0; i < sizeof(api_cmd_list) / sizeof(api_cmd_t); i++) {
        if (api_cmd_list[i].cmd == cmd) {
            api_cmd_list[i].handler(req);
            handled = true;
            break;
        }
    }

    if (!handled) {
        ESP_LOGW(TAG, "Unknown command: %d", cmd);
        api_send_response(req, "error", "Unknown command");
    }

    cJSON_Delete(root);
}
