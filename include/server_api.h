#ifndef _SERVER_API_H_
#define _SERVER_API_H_

#include <esp_err.h>
#include "server.h"

typedef enum {
    API_GET_STATUS = 0,
    API_SET_AP_SETTINGS,    /* 1 */
    API_GET_AP_SETTINGS,    /* 2 */
    API_REBOOT,             /* 3 */
    API_SET_STA_SETTINGS,   /* 4 */
    API_GET_STA_SETTINGS,   /* 5 */
    API_WPS_START,          /* 6 */
    API_WPS_STOP,           /* 7 */
    API_WPS_STATUS,         /* 8 */
    API_SAVED_WIFI_LIST,    /* 9 */
    API_SAVED_WIFI_DELETE,  /* 10 */
    API_SET_WEB_PASS,       /* 11 */
    API_MAX_COMMAND
} api_command_t;

const char* mime_from_path(const char* path);
void http_api_parse(ws_frame_req_t *req);

#endif
