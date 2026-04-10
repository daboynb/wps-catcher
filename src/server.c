#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_log.h>
#include <esp_random.h>
#include <lwip/sockets.h>
#include <cJSON.h>
#include "server.h"
#include "server_api.h"
#include "config.h"
#include "nvs_keys.h"


static const char *TAG = "WEBSERVER";
static httpd_handle_t server = NULL;
static QueueHandle_t ws_frame_queue = NULL;
static TaskHandle_t ws_frame_process_task_handle = NULL;

/* ---- Auth ---- */
static char auth_token[33] = {0};   /* 32 hex chars + null */
static char web_password[65] = {0};

static void auth_generate_token(void)
{
    uint32_t r[4];
    esp_fill_random(r, sizeof(r));
    snprintf(auth_token, sizeof(auth_token), "%08lx%08lx%08lx%08lx",
             (unsigned long)r[0], (unsigned long)r[1],
             (unsigned long)r[2], (unsigned long)r[3]);
}

static void auth_load_password(void)
{
    if (read_string_from_flash(WEB_PASS_KEY, web_password, sizeof(web_password)) != ESP_OK
        || web_password[0] == '\0') {
        strlcpy(web_password, config_default_ap_pass(), sizeof(web_password));
    }
    ESP_LOGI(TAG, "Web auth enabled (pass len=%d)", (int)strlen(web_password));
}

bool http_check_auth(httpd_req_t *req)
{
    if (auth_token[0] == '\0') return false;

    char cookie[256] = {0};
    if (httpd_req_get_hdr_value_str(req, "Cookie", cookie, sizeof(cookie)) != ESP_OK) {
        return false;
    }

    char *p = strstr(cookie, "session=");
    if (!p) return false;
    p += 8;

    return strncmp(p, auth_token, 32) == 0;
}

static bool is_public_path(const char *uri)
{
    return (strcmp(uri, "/login.html") == 0 ||
            strcmp(uri, "/css/common.css") == 0 ||
            strcmp(uri, "/api/login") == 0 ||
            /* Captive portal endpoints */
            strcmp(uri, "/hotspot-detect.html") == 0 ||
            strcmp(uri, "/library/test/success.html") == 0 ||
            strcmp(uri, "/generate_204") == 0 ||
            strcmp(uri, "/connecttest.txt") == 0 ||
            strcmp(uri, "/ncsi.txt") == 0 ||
            strcmp(uri, "/check_network_status.txt") == 0 ||
            strcmp(uri, "/canonical.html") == 0 ||
            strcmp(uri, "/redirect") == 0);
}

static esp_err_t send_redirect_to_login(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/login.html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}


/* ---- WebSocket ---- */

static void ws_send_work(void *arg)
{
    ws_frame_req_t *r = (ws_frame_req_t *)arg;
    httpd_ws_frame_t out = {
        .final = true,
        .fragmented = false,
        .type = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t *)r->payload,
        .len = r->len ? r->len : strlen(r->payload)
    };

    if (r->fd == -1) {
        /* BROADCAST */
        size_t fds = 10;
        int client_fds[10];
        if (httpd_get_client_list(r->hd, &fds, client_fds) == ESP_OK) {
            for (int i = 0; i < fds; i++) {
                if (httpd_ws_get_fd_info(r->hd, client_fds[i]) == HTTPD_WS_CLIENT_WEBSOCKET) {
                    esp_err_t ret = httpd_ws_send_frame_async(r->hd, client_fds[i], &out);
                    if (ret != ESP_OK) {
                        httpd_sess_trigger_close(r->hd, client_fds[i]);
                    }
                }
            }
        }
    } else {
        /* UNICAST */
        esp_err_t ret = httpd_ws_send_frame_async(r->hd, r->fd, &out);
        if (ret != ESP_OK) {
            httpd_sess_trigger_close(r->hd, r->fd);
        }
    }

    if (r->need_free && r->payload) free(r->payload);
    free(r);
}


static void ws_frame_process_task(void *pvParameter)
{
    (void)pvParameter;
    ws_frame_req_t ws_frame;

    while (1) {
        if (xQueueReceive(ws_frame_queue, &ws_frame, portMAX_DELAY) == pdTRUE) {
            switch (ws_frame.frame_type) {
            case WS_RX_FRAME:
                http_api_parse(&ws_frame);
                if (ws_frame.payload) free(ws_frame.payload);
                break;

            case WS_TX_FRAME: {
                ws_frame_req_t *heap_req = malloc(sizeof(ws_frame_req_t));
                if (heap_req) {
                    memcpy(heap_req, &ws_frame, sizeof(ws_frame_req_t));
                    if (httpd_queue_work(ws_frame.hd, ws_send_work, heap_req) != ESP_OK) {
                        if (heap_req->payload && heap_req->need_free) free(heap_req->payload);
                        free(heap_req);
                    }
                } else {
                    if (ws_frame.payload && ws_frame.need_free) free(ws_frame.payload);
                }
                break;
            }
            default:
                break;
            }
        }
    }
}


static esp_err_t captive_portal_redirect(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/login.html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_set_hdr(req, "Content-Length", "0");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}


static esp_err_t ws_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        /* Auth check on WebSocket handshake */
        if (!http_check_auth(req)) {
            httpd_resp_set_status(req, "401 Unauthorized");
            httpd_resp_send(req, "Unauthorized", HTTPD_RESP_USE_STRLEN);
            return ESP_FAIL;
        }
        ESP_LOGD(TAG, "WS handshake, fd=%d", httpd_req_to_sockfd(req));
        return ESP_OK;
    }

    httpd_ws_frame_t frame = {0};
    frame.type = HTTPD_WS_TYPE_TEXT;

    esp_err_t ret = httpd_ws_recv_frame(req, &frame, 0);
    if (ret != ESP_OK) return ret;

    if (frame.type == HTTPD_WS_TYPE_PING) {
        httpd_ws_frame_t pong = {.type = HTTPD_WS_TYPE_PONG, .payload = NULL, .len = 0};
        return httpd_ws_send_frame(req, &pong);
    }

    if (frame.type == HTTPD_WS_TYPE_CLOSE) return ESP_OK;

    if (frame.len > 0) {
        char *buf = malloc(frame.len + 1);
        if (!buf) return ESP_ERR_NO_MEM;

        frame.payload = (uint8_t *)buf;
        ret = httpd_ws_recv_frame(req, &frame, frame.len);
        if (ret != ESP_OK) {
            free(buf);
            return ret;
        }
        buf[frame.len] = 0;

        ws_frame_req_t ws_req = {
            .hd = req->handle,
            .fd = httpd_req_to_sockfd(req),
            .frame_type = WS_RX_FRAME,
            .payload = buf,
            .len = frame.len,
            .need_free = true
        };

        if (xQueueSend(ws_frame_queue, &ws_req, 100) != pdTRUE) {
            free(buf);
            return ESP_FAIL;
        }
    }
    return ESP_OK;
}


/* ---- Login API ---- */

static esp_err_t login_handler(httpd_req_t *req)
{
    char body[128] = {0};
    int len = httpd_req_recv(req, body, sizeof(body) - 1);
    if (len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_FAIL;
    }
    body[len] = '\0';

    cJSON *json = cJSON_Parse(body);
    if (!json) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    const cJSON *j_pass = cJSON_GetObjectItemCaseSensitive(json, "password");
    bool ok = false;

    if (cJSON_IsString(j_pass) && strcmp(j_pass->valuestring, web_password) == 0) {
        ok = true;
    }
    cJSON_Delete(json);

    if (!ok) {
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"Wrong password\"}");
        return ESP_OK;
    }

    /* Generate new session token */
    auth_generate_token();

    /* Set cookie */
    char cookie[80];
    snprintf(cookie, sizeof(cookie), "session=%s; Path=/; HttpOnly; SameSite=Strict", auth_token);
    httpd_resp_set_hdr(req, "Set-Cookie", cookie);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"ok\"}");

    ESP_LOGI(TAG, "Login successful");
    return ESP_OK;
}


static esp_err_t logout_handler(httpd_req_t *req)
{
    /* Invalidate token */
    auth_generate_token();

    httpd_resp_set_hdr(req, "Set-Cookie",
        "session=; Path=/; HttpOnly; SameSite=Strict; Max-Age=0");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"ok\"}");

    ESP_LOGI(TAG, "Logout");
    return ESP_OK;
}


/* ---- File server ---- */

static esp_err_t file_handler(httpd_req_t *req)
{
    char filepath[128] = "/spiffs";
    char buf[1024];
    size_t n;
    const char *uri = req->uri;

    /* Captive portal detection endpoints */
    if (strcmp(uri, "/hotspot-detect.html") == 0 ||
        strcmp(uri, "/library/test/success.html") == 0 ||
        strcmp(uri, "/generate_204") == 0 ||
        strcmp(uri, "/connecttest.txt") == 0 ||
        strcmp(uri, "/ncsi.txt") == 0 ||
        strcmp(uri, "/check_network_status.txt") == 0 ||
        strcmp(uri, "/canonical.html") == 0 ||
        strcmp(uri, "/redirect") == 0) {
        return captive_portal_redirect(req);
    }

    /* Auth check — redirect to login if not authenticated */
    if (!is_public_path(uri) && !http_check_auth(req)) {
        return send_redirect_to_login(req);
    }

    if (strcmp(uri, "/") == 0) {
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", "/wps.html");
        httpd_resp_send(req, NULL, 0);
        return ESP_OK;
    }

    strlcat(filepath, uri, sizeof(filepath));

    FILE *f = fopen(filepath, "r");
    if (!f) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not found");
        return ESP_OK;
    }

    httpd_resp_set_type(req, mime_from_path(filepath));

    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        if (httpd_resp_send_chunk(req, buf, n) != ESP_OK) {
            fclose(f);
            return ESP_FAIL;
        }
    }
    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}


/* ---- Queue helpers ---- */

esp_err_t ws_send_command_to_queue(ws_frame_req_t *_req)
{
    if (ws_frame_queue == NULL) return ESP_FAIL;
    _req->frame_type = WS_TX_FRAME;
    if (xQueueSend(ws_frame_queue, _req, 100) != pdTRUE) return ESP_FAIL;
    return ESP_OK;
}


esp_err_t ws_send_broadcast_to_queue(ws_frame_req_t *_req)
{
    if (ws_frame_queue == NULL) return ESP_FAIL;
    if (uxQueueSpacesAvailable(ws_frame_queue) == 0) return ESP_FAIL;
    _req->frame_type = WS_TX_FRAME;
    if (xQueueSend(ws_frame_queue, _req, 0) != pdTRUE) return ESP_FAIL;
    return ESP_OK;
}


/* ---- Server start/stop ---- */

void http_server_start(void)
{
    if (server != NULL) return;

    /* Load web auth password */
    auth_load_password();
    auth_generate_token();

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.ctrl_port = 81;
    config.server_port = 80;
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.max_open_sockets = 7;
    config.max_resp_headers = 8;
    config.recv_wait_timeout = 10;
    config.send_wait_timeout = 10;
    config.lru_purge_enable = true;
    config.max_uri_handlers = 12;

    ws_frame_queue = xQueueCreate(WS_FRAME_QUEUE_LENGTH, sizeof(ws_frame_req_t));
    if (ws_frame_queue == NULL) return;
    xTaskCreate(ws_frame_process_task, "ws_process", 4096, NULL, 5, &ws_frame_process_task_handle);

    esp_err_t ret = httpd_start(&server, &config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Server start failed: %s", esp_err_to_name(ret));
        return;
    }

    /* Login/logout API */
    httpd_uri_t login_uri = {
        .uri = "/api/login", .method = HTTP_POST, .handler = login_handler
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &login_uri));

    httpd_uri_t logout_uri = {
        .uri = "/api/logout", .method = HTTP_POST, .handler = logout_handler
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &logout_uri));

    /* WebSocket endpoint */
    httpd_uri_t ws_uri = {
        .uri = "/ws",
        .method = HTTP_GET,
        .handler = ws_handler,
        .is_websocket = true,
        .handle_ws_control_frames = true
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &ws_uri));

    /* Catch-all file server */
    httpd_uri_t any = {
        .uri = "/*",
        .method = HTTP_GET,
        .handler = file_handler
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &any));

    ESP_LOGI(TAG, "HTTP server started on port 80");
}


void http_server_stop(void)
{
    if (server) {
        httpd_stop(server);
        server = NULL;
    }
    if (ws_frame_process_task_handle) {
        vTaskDelete(ws_frame_process_task_handle);
        ws_frame_process_task_handle = NULL;
    }
    if (ws_frame_queue) {
        vQueueDelete(ws_frame_queue);
        ws_frame_queue = NULL;
    }
}


httpd_handle_t get_web_server_handle(void)
{
    return server;
}
