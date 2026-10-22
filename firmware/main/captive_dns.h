/* Setup-only IPv4 DNS responder. Lifecycle calls belong to the provisioning task. */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

esp_err_t rw_captive_dns_start(uint32_t ap_address);
void rw_captive_dns_stop(void);

/* In-place single-question DNS reply. Address is in network byte order.
 * Returns 0 for malformed/unsupported packets or insufficient capacity. */
size_t rw_captive_dns_reply(uint8_t *packet, size_t length, size_t capacity,
                           uint32_t ap_address);
