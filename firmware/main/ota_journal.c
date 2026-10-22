#include "ota_journal.h"
#include <string.h>
bool rw_ota_journal_valid(const rw_ota_journal_t *j, const uint8_t scope[32])
{
    return j->format == RW_OTA_JOURNAL_VERSION && !memcmp(j->scope, scope, 32) &&
        j->failed_count <= RW_OTA_FAILED_HASHES && j->failed_next < RW_OTA_FAILED_HASHES;
}
bool rw_ota_journal_rejected(const rw_ota_journal_t *j, const uint8_t hash[32])
{
    for (unsigned i = 0; i < j->failed_count; ++i) if (!memcmp(j->failed[i], hash, 32)) return true;
    return false;
}
void rw_ota_journal_clear_pending(rw_ota_journal_t *j)
{
    j->pending_release = j->pending_address = 0;
    memset(j->pending_hash, 0, sizeof(j->pending_hash));
}
void rw_ota_journal_prepare(rw_ota_journal_t *j, uint32_t release, uint32_t address, const uint8_t hash[32])
{
    j->pending_release = release; j->pending_address = address; memcpy(j->pending_hash, hash, 32);
}
void rw_ota_journal_reject(rw_ota_journal_t *j, const uint8_t hash[32])
{
    if (!rw_ota_journal_rejected(j, hash)) {
        memcpy(j->failed[j->failed_next], hash, 32);
        j->failed_next = (j->failed_next + 1) % RW_OTA_FAILED_HASHES;
        if (j->failed_count < RW_OTA_FAILED_HASHES) ++j->failed_count;
    }
    rw_ota_journal_clear_pending(j);
}
void rw_ota_journal_confirm(rw_ota_journal_t *j, uint32_t release)
{
    if (release > j->floor) j->floor = release;
    rw_ota_journal_clear_pending(j);
}
