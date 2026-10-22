#pragma once
#include <stdint.h>
#include "esp_err.h"
typedef struct esp_netif_obj esp_netif_t;
typedef struct { uint32_t addr; } esp_ip4_addr_t;
typedef struct { esp_ip4_addr_t ip, netmask, gw; } esp_netif_ip_info_t;
#define ESP_IPADDR_TYPE_V4 0
typedef struct { union { esp_ip4_addr_t ip4; } u_addr; uint8_t type; } esp_ip_addr_t;
typedef struct { esp_ip_addr_t ip; } esp_netif_dns_info_t;
typedef enum { ESP_NETIF_DNS_MAIN } esp_netif_dns_type_t;
typedef enum { ESP_NETIF_OP_SET } esp_netif_dhcp_option_mode_t;
typedef enum { ESP_NETIF_DOMAIN_NAME_SERVER } esp_netif_dhcp_option_id_t;
#define IPSTR "%d.%d.%d.%d"
#define IP2STR(a) (int)((a)->addr & 0xff), (int)(((a)->addr >> 8) & 0xff), \
                  (int)(((a)->addr >> 16) & 0xff), (int)(((a)->addr >> 24) & 0xff)
esp_netif_t *esp_netif_create_default_wifi_ap(void);
void esp_netif_destroy_default_wifi(void *netif);
esp_err_t esp_netif_get_ip_info(esp_netif_t *netif, esp_netif_ip_info_t *info);
esp_err_t esp_netif_dhcps_stop(esp_netif_t *netif);
esp_err_t esp_netif_dhcps_start(esp_netif_t *netif);
esp_err_t esp_netif_dhcps_option(esp_netif_t *netif, esp_netif_dhcp_option_mode_t op,
                                 esp_netif_dhcp_option_id_t id, void *val, uint32_t len);
esp_err_t esp_netif_set_dns_info(esp_netif_t *netif, esp_netif_dns_type_t type,
                                 esp_netif_dns_info_t *dns);
