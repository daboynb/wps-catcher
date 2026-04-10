#ifndef _WIFI_WPS_H_
#define _WIFI_WPS_H_

#include <esp_err.h>
#include <esp_wifi_types.h>

typedef enum {
    WPS_STATE_IDLE = 0,
    WPS_STATE_LISTENING,
    WPS_STATE_SUCCESS,
    WPS_STATE_FAILED,
    WPS_STATE_TIMEOUT
} wps_state_t;

typedef struct {
    wps_state_t state;
    uint8_t target_bssid[6];   /* filter: 00:00:00:00:00:00 = any */
    bool    has_target;
    int     timeout_sec;       /* 0 = no timeout */
    int     elapsed_sec;
    char    captured_ssid[33];
    char    captured_pass[65];
    uint8_t captured_bssid[6];
} wps_status_t;

/* WPS PBC listener */
esp_err_t wifi_wps_listen_start(const uint8_t *target_bssid, int timeout_sec);
void wifi_wps_listen_stop(void);
const wps_status_t *wifi_wps_get_status(void);

#endif
