idf_component_register(
    SRCS
        "captive_portal.c"
        "captive_http.c"
        "captive_dns_server.c"
        "captive_dns_packet.c"

    INCLUDE_DIRS
        "include"

    PRIV_INCLUDE_DIRS
        "."

    REQUIRES
        web_server
        esp_http_server
        esp_netif
        lwip
        freertos
        log
)