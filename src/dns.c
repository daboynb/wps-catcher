#include <string.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include "esp_log.h"


#define DNS_PORT 53
#define RESPONSE_IP_ADDR {192, 168, 4, 1}

static const char *TAG = "DNS";
static TaskHandle_t dns_task_handle = NULL;
static volatile bool dns_running = false;


static void dns_server_task(void *pvParameters)
{
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_addr_len = sizeof(client_addr);
    char buf[512];

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        ESP_LOGE(TAG, "Socket creation failed");
        dns_running = false;
        vTaskDelete(NULL);
        return;
    }

    /* Set recv timeout so the task can check the stop flag */
    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port = htons(DNS_PORT);

    if (bind(sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        ESP_LOGE(TAG, "Bind failed");
        close(sock);
        dns_running = false;
        vTaskDelete(NULL);
        return;
    }

    uint8_t response_template[] = {
        0x00, 0x00,
        0x81, 0x80,
        0x00, 0x01,
        0x00, 0x01,
        0x00, 0x00,
        0x00, 0x00,
    };
    const uint8_t ip_address[] = RESPONSE_IP_ADDR;

    while (dns_running) {
        int len = recvfrom(sock, buf, sizeof(buf), 0,
                           (struct sockaddr *)&client_addr, &client_addr_len);
        if (len < 12) continue;

        response_template[0] = buf[0];
        response_template[1] = buf[1];

        uint8_t response[600];
        memcpy(response, response_template, sizeof(response_template));
        int response_len = sizeof(response_template);

        int question_len = len - 12;
        if (sizeof(response_template) + question_len + 16 > sizeof(response)) continue;

        memcpy(&response[response_len], &buf[12], question_len);
        response_len += question_len;

        uint8_t answer[] = {
            0xC0, 0x0C,
            0x00, 0x01,
            0x00, 0x01,
            0x00, 0x00, 0x00, 0x3C,
            0x00, 0x04,
            ip_address[0], ip_address[1], ip_address[2], ip_address[3]
        };

        memcpy(&response[response_len], answer, sizeof(answer));
        response_len += sizeof(answer);
        sendto(sock, response, response_len, 0,
               (struct sockaddr *)&client_addr, client_addr_len);
    }

    close(sock);
    dns_task_handle = NULL;
    vTaskDelete(NULL);
}


void dns_server_start(void)
{
    if (dns_task_handle != NULL) return;
    dns_running = true;
    xTaskCreate(dns_server_task, "dns_server", 4096, NULL, 5, &dns_task_handle);
}


void dns_server_stop(void)
{
    if (dns_task_handle == NULL) return;
    dns_running = false;
    /* Wait for task to exit gracefully (recv timeout is 1s) */
    vTaskDelay(pdMS_TO_TICKS(1500));
    if (dns_task_handle != NULL) {
        vTaskDelete(dns_task_handle);
        dns_task_handle = NULL;
    }
}
