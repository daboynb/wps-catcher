#ifndef _CONFIG_H
#define _CONFIG_H

#include <esp_system.h>
#include <esp_wifi.h>

void save_string_to_flash(const char* key, const char* value);
esp_err_t read_string_from_flash(const char* key, char* value, size_t max_len);
void save_int_to_flash(const char* key, int32_t value);
esp_err_t read_int_from_flash(const char* key, int32_t *value);
void config_load_json(void);

/* Defaults loaded from config.json (fallback if JSON missing) */
const char *config_default_ap_ssid(void);
const char *config_default_ap_pass(void);
int config_default_ap_channel(void);

#endif
