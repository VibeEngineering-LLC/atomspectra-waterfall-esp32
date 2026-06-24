#include "atomspectra.h"
#include "esp_log.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>
#include <errno.h>

static const char *TAG = "tcp_bridge";

static int s_client_fd = -1;
static int s_server_fd = -1;

// P2-4: s_client_fd трогают три задачи (usb_to_tcp_cb send / tcp_rx_task
// recv+close / tcp_server_task accept). Мьютекс закрывает use-after-close;
// блокирующий recv() делаем вне лока.
static SemaphoreHandle_t s_fd_mutex = NULL;
#define FD_LOCK()   do { if (s_fd_mutex) xSemaphoreTake(s_fd_mutex, portMAX_DELAY); } while (0)
#define FD_UNLOCK() do { if (s_fd_mutex) xSemaphoreGive(s_fd_mutex); } while (0)

static void usb_to_tcp_cb(const uint8_t *data, size_t len)
{
    FD_LOCK();
    int fd = s_client_fd;
    if (fd >= 0) {
        send(fd, data, len, MSG_DONTWAIT);
    }
    FD_UNLOCK();
}

static void tcp_rx_task(void *arg)
{
    uint8_t buf[1024];
    while (1) {
        FD_LOCK();
        int fd = s_client_fd;
        FD_UNLOCK();
        if (fd < 0) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        int n = recv(fd, buf, sizeof(buf), 0);   // блокирующий recv — вне лока
        if (n <= 0) {
            ESP_LOGI(TAG, "Client disconnected");
            FD_LOCK();
            close(fd);
            s_client_fd = -1;
            FD_UNLOCK();
            continue;
        }
        usb_host_cdc_send(buf, n);
    }
}

static void tcp_server_task(void *arg)
{
    s_server_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s_server_fd < 0) {
        ESP_LOGE(TAG, "Socket create failed");
        vTaskDelete(NULL);
        return;
    }

    int opt = 1;
    if (setsockopt(s_server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0)
        ESP_LOGW(TAG, "SO_REUSEADDR failed: errno=%d", errno);

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(TCP_BRIDGE_PORT),
        .sin_addr.s_addr = INADDR_ANY
    };
    if (bind(s_server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "Bind port %d failed", TCP_BRIDGE_PORT);
        close(s_server_fd);
        vTaskDelete(NULL);
        return;
    }
    if (listen(s_server_fd, 1) < 0) {
        ESP_LOGE(TAG, "Listen on port %d failed: errno=%d", TCP_BRIDGE_PORT, errno);
        close(s_server_fd);
        s_server_fd = -1;
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "Listening on port %d", TCP_BRIDGE_PORT);

    while (1) {
        struct sockaddr_in client_addr;
        socklen_t clen = sizeof(client_addr);
        int fd = accept(s_server_fd, (struct sockaddr *)&client_addr, &clen);
        if (fd < 0) continue;

        FD_LOCK();
        bool busy = (s_client_fd >= 0);
        FD_UNLOCK();
        if (busy) {
            ESP_LOGW(TAG, "Rejecting second client");
            close(fd);
            continue;
        }

        int nodelay = 1;
        if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay)) < 0)
            ESP_LOGW(TAG, "TCP_NODELAY failed: errno=%d", errno);

        FD_LOCK();
        s_client_fd = fd;
        FD_UNLOCK();
        ESP_LOGI(TAG, "Client connected from %s", inet_ntoa(client_addr.sin_addr));
    }
}

void tcp_bridge_init(void)
{
    s_fd_mutex = xSemaphoreCreateMutex();
    usb_host_cdc_set_raw_rx_cb(usb_to_tcp_cb);
    xTaskCreate(tcp_server_task, "tcp_srv", 4096, NULL, 5, NULL);
    xTaskCreate(tcp_rx_task, "tcp_rx", 4096, NULL, 5, NULL);
    ESP_LOGI(TAG, "TCP bridge initialized, port %d", TCP_BRIDGE_PORT);
}

bool tcp_bridge_client_connected(void)
{
    return s_client_fd >= 0;
}