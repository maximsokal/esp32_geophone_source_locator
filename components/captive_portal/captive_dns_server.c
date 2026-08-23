#include "captive_dns_server.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

#include "captive_dns_packet.h"

#include "esp_log.h"
#include "esp_netif.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "lwip/inet.h"
#include "lwip/sockets.h"


static const char *TAG =
    "CAPTIVE_DNS";


#define CAPTIVE_DNS_PORT             53
#define CAPTIVE_DNS_PACKET_SIZE      512
#define CAPTIVE_DNS_TASK_STACK       4096
#define CAPTIVE_DNS_TASK_PRIORITY    5

#define CAPTIVE_DNS_RECEIVE_MS       250


static volatile bool s_running =
    false;


static TaskHandle_t s_task =
    NULL;


static void captive_dns_server_task(
    void *context
)
{
    (void)context;


    esp_netif_t *ap_netif =
        esp_netif_get_handle_from_ifkey(
            "WIFI_AP_DEF"
        );


    if (ap_netif == NULL) {

        ESP_LOGE(
            TAG,
            "Wi-Fi AP network interface not found"
        );

        s_running = false;
        s_task = NULL;

        vTaskDelete(NULL);

        return;
    }


    esp_netif_ip_info_t ip_info;


    if (
        esp_netif_get_ip_info(
            ap_netif,
            &ip_info
        ) != ESP_OK
    ) {
        ESP_LOGE(
            TAG,
            "Failed to read AP IP address"
        );

        s_running = false;
        s_task = NULL;

        vTaskDelete(NULL);

        return;
    }


    char ip_address[16];


    inet_ntoa_r(
        ip_info.ip.addr,
        ip_address,
        sizeof(ip_address)
    );


    int socket_fd =
        socket(
            AF_INET,
            SOCK_DGRAM,
            IPPROTO_IP
        );


    if (socket_fd < 0) {

        ESP_LOGE(
            TAG,
            "Failed to create DNS socket: errno=%d",
            errno
        );

        s_running = false;
        s_task = NULL;

        vTaskDelete(NULL);

        return;
    }


    const struct timeval receive_timeout = {
        .tv_sec = 0,
        .tv_usec =
            CAPTIVE_DNS_RECEIVE_MS *
            1000,
    };


    setsockopt(
        socket_fd,
        SOL_SOCKET,
        SO_RCVTIMEO,
        &receive_timeout,
        sizeof(receive_timeout)
    );


    struct sockaddr_in server_address = {
        .sin_family = AF_INET,
        .sin_port =
            htons(
                CAPTIVE_DNS_PORT
            ),
        .sin_addr = {
            .s_addr =
                htonl(
                    INADDR_ANY
                ),
        },
    };


    if (
        bind(
            socket_fd,
            (struct sockaddr *)&server_address,
            sizeof(server_address)
        ) < 0
    ) {
        ESP_LOGE(
            TAG,
            "Failed to bind DNS port 53: errno=%d",
            errno
        );

        close(socket_fd);

        s_running = false;
        s_task = NULL;

        vTaskDelete(NULL);

        return;
    }


    ESP_LOGI(
        TAG,
        "DNS redirect server started on port 53"
    );


    ESP_LOGI(
        TAG,
        "All IPv4 DNS queries redirect to %s",
        ip_address
    );


    uint8_t request[
        CAPTIVE_DNS_PACKET_SIZE
    ];


    uint8_t response[
        CAPTIVE_DNS_PACKET_SIZE
    ];


    while (s_running) {

        struct sockaddr_storage
            client_address;


        socklen_t client_address_length =
            sizeof(client_address);


        const int received =
            recvfrom(
                socket_fd,
                request,
                sizeof(request),
                0,
                (struct sockaddr *)
                    &client_address,
                &client_address_length
            );


        if (received < 0) {

            if (
                errno == EAGAIN ||
                errno == EWOULDBLOCK
            ) {
                continue;
            }


            ESP_LOGW(
                TAG,
                "DNS receive failed: errno=%d",
                errno
            );

            continue;
        }


        const int response_length =
            captive_dns_packet_build_response(
                request,
                (size_t)received,
                ip_info.ip.addr,
                response,
                sizeof(response)
            );


        if (response_length <= 0) {
            continue;
        }


        const int sent =
            sendto(
                socket_fd,
                response,
                response_length,
                0,
                (struct sockaddr *)
                    &client_address,
                client_address_length
            );


        if (sent < 0) {

            ESP_LOGW(
                TAG,
                "DNS response failed: errno=%d",
                errno
            );
        }
    }


    close(socket_fd);


    ESP_LOGI(
        TAG,
        "DNS redirect server stopped"
    );


    s_running = false;
    s_task = NULL;


    vTaskDelete(NULL);
}


esp_err_t captive_dns_server_start(void)
{
    if (s_running) {
        return ESP_OK;
    }


    s_running =
        true;


    const BaseType_t result =
        xTaskCreate(
            captive_dns_server_task,
            "captive_dns",
            CAPTIVE_DNS_TASK_STACK,
            NULL,
            CAPTIVE_DNS_TASK_PRIORITY,
            &s_task
        );


    if (result != pdPASS) {

        s_running = false;
        s_task = NULL;

        ESP_LOGE(
            TAG,
            "Failed to create DNS task"
        );

        return ESP_ERR_NO_MEM;
    }


    return ESP_OK;
}


void captive_dns_server_stop(void)
{
    if (
        !s_running &&
        s_task == NULL
    ) {
        return;
    }


    s_running =
        false;


    /*
     * recvfrom uses a short timeout, so the task normally
     * exits by itself within 250 ms.
     */
    for (
        int attempt = 0;
        attempt < 50;
        ++attempt
    ) {
        if (s_task == NULL) {
            return;
        }


        vTaskDelay(
            pdMS_TO_TICKS(20)
        );
    }


    /*
     * This should not normally be reached.
     */
    if (s_task != NULL) {

        ESP_LOGW(
            TAG,
            "DNS task did not stop in time"
        );


        vTaskDelete(
            s_task
        );


        s_task = NULL;
    }
}


bool captive_dns_server_is_running(void)
{
    return s_running;
}