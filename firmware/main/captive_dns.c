#include "captive_dns.h"
#include <errno.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "lwip/sockets.h"

static TaskHandle_t s_task;
static SemaphoreHandle_t s_done;
static int s_socket = -1;
static uint32_t s_address;

static void dns_task(void *arg)
{
    (void)arg;
    uint8_t packet[512];
    while (!ulTaskNotifyTake(pdTRUE, 0)) {
        struct sockaddr_in peer;
        socklen_t peer_len = sizeof peer;
        int n = recvfrom(s_socket, packet, sizeof packet, 0,
                         (struct sockaddr *)&peer, &peer_len);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
                continue;
            break;
        }
        size_t reply = rw_captive_dns_reply(packet, (size_t)n, sizeof packet, s_address);
        if (reply)
            sendto(s_socket, packet, reply, 0, (struct sockaddr *)&peer, peer_len);
    }
    close(s_socket);
    s_socket = -1;
    xSemaphoreGive(s_done);
    /* Keep the handle valid until the owner deletes us, including fatal I/O errors. */
    vTaskSuspend(NULL);
}

esp_err_t rw_captive_dns_start(uint32_t address)
{
    if (s_task)
        return ESP_ERR_INVALID_STATE;
    s_done = xSemaphoreCreateBinary();
    if (!s_done)
        return ESP_ERR_NO_MEM;
    s_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_socket < 0)
        goto fail;
    struct timeval timeout = {.tv_usec = 200000};
    struct sockaddr_in local = {
        .sin_family = AF_INET, .sin_port = htons(53), .sin_addr.s_addr = address,
    };
    if (setsockopt(s_socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout) < 0 ||
        setsockopt(s_socket, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof timeout) < 0 ||
        bind(s_socket, (struct sockaddr *)&local, sizeof local) < 0)
        goto fail;
    s_address = address;
    if (xTaskCreate(dns_task, "rw_setup_dns", 3072, NULL, 4, &s_task) != pdPASS)
        goto fail;
    return ESP_OK;
fail:
    if (s_socket >= 0)
        close(s_socket);
    s_socket = -1;
    vSemaphoreDelete(s_done);
    s_done = NULL;
    return ESP_FAIL;
}

void rw_captive_dns_stop(void)
{
    if (!s_task)
        return;
    xTaskNotifyGive(s_task);
    xSemaphoreTake(s_done, portMAX_DELAY);
    vTaskDelete(s_task);
    s_task = NULL;
    vSemaphoreDelete(s_done);
    s_done = NULL;
}
