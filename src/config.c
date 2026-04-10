#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <cJSON.h>
#include "nvs.h"
#include "esp_log.h"
#include "config.h"
#include "nvs_keys.h"


static const char *TAG = "CONFIG";

/* Defaults — loaded from config.json, with hardcoded fallback */
static char def_ap_ssid[32] = "ESP32-Tool";
static char def_ap_pass[64] = "esp32tool!";
static int  def_ap_channel  = 1;

const char *config_default_ap_ssid(void) { return def_ap_ssid; }
const char *config_default_ap_pass(void) { return def_ap_pass; }
int config_default_ap_channel(void) { return def_ap_channel; }


void save_string_to_flash(const char* key, const char* value)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("storage", NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS open failed: %s", esp_err_to_name(err));
        return;
    }

    err = nvs_set_str(nvs_handle, key, value);
    if (err == ESP_OK) {
        nvs_commit(nvs_handle);
    }
    nvs_close(nvs_handle);
}


esp_err_t read_string_from_flash(const char* key, char* value, size_t max_len)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("storage", NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) return ESP_FAIL;

    size_t required_size = 0;
    err = nvs_get_str(nvs_handle, key, NULL, &required_size);
    if (err != ESP_OK || required_size > max_len) {
        nvs_close(nvs_handle);
        return ESP_FAIL;
    }

    err = nvs_get_str(nvs_handle, key, value, &required_size);
    nvs_close(nvs_handle);
    return err;
}


void save_int_to_flash(const char* key, int32_t value)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("storage", NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS open failed: %s", esp_err_to_name(err));
        return;
    }

    err = nvs_set_i32(nvs_handle, key, value);
    if (err == ESP_OK) {
        nvs_commit(nvs_handle);
    }
    nvs_close(nvs_handle);
}


esp_err_t read_int_from_flash(const char* key, int32_t *value)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("storage", NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) return ESP_FAIL;

    err = nvs_get_i32(nvs_handle, key, value);
    nvs_close(nvs_handle);
    return err;
}


void config_load_json(void)
{
    FILE *f = fopen("/spiffs/json/config.json", "r");
    if (!f) {
        ESP_LOGI(TAG, "No config.json found, using NVS/defaults");
        return;
    }

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    char *buf = malloc(fsize + 1);
    if (!buf) { fclose(f); return; }
    fread(buf, 1, fsize, f);
    buf[fsize] = 0;
    fclose(f);

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) {
        ESP_LOGE(TAG, "config.json parse error");
        return;
    }

    /* AP settings */
    cJSON *ap = cJSON_GetObjectItemCaseSensitive(root, "ap");
    if (ap) {
        cJSON *ssid = cJSON_GetObjectItemCaseSensitive(ap, "ssid");
        cJSON *pass = cJSON_GetObjectItemCaseSensitive(ap, "password");
        cJSON *chan = cJSON_GetObjectItemCaseSensitive(ap, "channel");

        if (cJSON_IsString(ssid) && ssid->valuestring[0]) {
            strlcpy(def_ap_ssid, ssid->valuestring, sizeof(def_ap_ssid));
            save_string_to_flash(WIFI_SSID_KEY, ssid->valuestring);
            ESP_LOGI(TAG, "config.json: AP ssid='%s'", ssid->valuestring);
        }
        if (cJSON_IsString(pass) && pass->valuestring[0]) {
            strlcpy(def_ap_pass, pass->valuestring, sizeof(def_ap_pass));
            save_string_to_flash(WIFI_PASS_KEY, pass->valuestring);
        }
        if (cJSON_IsNumber(chan)) {
            def_ap_channel = chan->valueint;
            save_int_to_flash(WIFI_CHAN_KEY, chan->valueint);
        }
    }

    /* STA settings */
    cJSON *sta = cJSON_GetObjectItemCaseSensitive(root, "sta");
    if (sta) {
        cJSON *ssid = cJSON_GetObjectItemCaseSensitive(sta, "ssid");
        cJSON *pass = cJSON_GetObjectItemCaseSensitive(sta, "password");

        if (cJSON_IsString(ssid)) {
            save_string_to_flash(STA_SSID_KEY, ssid->valuestring);
            if (ssid->valuestring[0]) {
                ESP_LOGI(TAG, "config.json: STA ssid='%s'", ssid->valuestring);
            }
        }
        if (cJSON_IsString(pass)) {
            save_string_to_flash(STA_PASS_KEY, pass->valuestring);
        }
    }

    /* Web UI password */
    cJSON *web = cJSON_GetObjectItemCaseSensitive(root, "web_pass");
    if (cJSON_IsString(web) && web->valuestring[0]) {
        save_string_to_flash(WEB_PASS_KEY, web->valuestring);
        ESP_LOGI(TAG, "config.json: web_pass set");
    }

    cJSON_Delete(root);
    ESP_LOGI(TAG, "config.json loaded into NVS");
}
