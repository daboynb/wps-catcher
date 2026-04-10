#ifndef _WIFI_MANAGER_H
#define _WIFI_MANAGER_H

#include <esp_err.h>
#include <esp_wifi.h>

esp_err_t wifi_init(void);
void wifi_start(void);
void wifi_start_softap(void);
void wifi_start_sta(void);
void wifi_connect_sta(void);
bool wifi_sta_is_connected(void);
esp_err_t wifi_get_sta_ip(char *buf, int buf_len);
void wifi_sta_set_reconnect(bool enable);

#endif
