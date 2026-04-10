#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_task_wdt.h>
#include <esp_log.h>
#include <nvs_flash.h>
#include <esp_spiffs.h>
#include "config.h"
#include "server.h"
#include "wifi_manager.h"
#include "dns.h"

void app_main()
{
    esp_err_t ret = ESP_OK;
    esp_task_wdt_deinit();

    ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    esp_vfs_spiffs_conf_t spiffs_conf = {
        .base_path = "/spiffs",
        .partition_label = NULL,
        .max_files = 5,
        .format_if_mount_failed = true
    };
    ret = esp_vfs_spiffs_register(&spiffs_conf);
    if (ret != ESP_OK) {
        ESP_LOGE("MAIN", "SPIFFS mount failed: %s", esp_err_to_name(ret));
        return;
    }

    config_load_json();

    if (wifi_init() != ESP_OK) {
        ESP_LOGE("MAIN", "WiFi init failed");
        return;
    }

    wifi_start();
    wifi_start_softap();
    wifi_connect_sta();
    dns_server_start();
    http_server_start();

    uint32_t idle_core_mask = (1 << portNUM_PROCESSORS) - 1;
    const esp_task_wdt_config_t wdt_conf = {
        .idle_core_mask = idle_core_mask,
        .timeout_ms = 10000,
        .trigger_panic = false
    };
    ESP_ERROR_CHECK(esp_task_wdt_init(&wdt_conf));
    vTaskSuspend(NULL);
}
