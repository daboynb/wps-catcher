#ifndef _SERVER_H
#define _SERVER_H

#include <esp_http_server.h>

#define WS_FRAME_BUFFER_SIZE 1024
#define WS_FRAME_QUEUE_LENGTH 64

typedef enum {
    WS_RX_FRAME = 0,
    WS_TX_FRAME
} ws_frame_type_t;


typedef struct {
    ws_frame_type_t frame_type;
    httpd_handle_t hd;
    int fd;
    uint32_t req_id;
    size_t len;
    char *payload;
    bool need_free;
} ws_frame_req_t;


esp_err_t ws_send_command_to_queue(ws_frame_req_t *_req);
esp_err_t ws_send_broadcast_to_queue(ws_frame_req_t *_req);
void http_server_start(void);
void http_server_stop(void);
httpd_handle_t get_web_server_handle(void);
bool http_check_auth(httpd_req_t *req);

#endif
