/* Durable journal transitions, independent of flash/network/RTOS. */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#define RW_OTA_FAILED_HASHES 4
#define RW_OTA_JOURNAL_VERSION 1
typedef struct {
    uint32_t format;
    uint8_t scope[32];
    uint32_t floor, pending_release, pending_address;
    uint8_t pending_hash[32], failed[RW_OTA_FAILED_HASHES][32];
    uint32_t failed_count, failed_next;
} rw_ota_journal_t;
bool rw_ota_journal_valid(const rw_ota_journal_t *j, const uint8_t scope[32]);
bool rw_ota_journal_rejected(const rw_ota_journal_t *j, const uint8_t hash[32]);
void rw_ota_journal_prepare(rw_ota_journal_t *j, uint32_t release, uint32_t address, const uint8_t hash[32]);
void rw_ota_journal_clear_pending(rw_ota_journal_t *j);
void rw_ota_journal_reject(rw_ota_journal_t *j, const uint8_t hash[32]);
void rw_ota_journal_confirm(rw_ota_journal_t *j, uint32_t release);
