#include "ota.h"
#include "sdkconfig.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include <string.h>

const char *rw_ota_phase_name(rw_ota_phase_t p)
{
    static const char *const names[] = {"disabled","idle","checking","available","downloading","verifying",
        "waiting_restart","restarting","pending_health","healthy","rolled_back","failed"};
    return (unsigned)p < sizeof(names)/sizeof(names[0]) ? names[p] : "failed";
}
static bool boot_pending(void)
{
    esp_ota_img_states_t state;
    const esp_partition_t *running = esp_ota_get_running_partition();
    return running && esp_ota_get_state_partition(running, &state) == ESP_OK && state == ESP_OTA_IMG_PENDING_VERIFY;
}
/* A factory-less USB bootstrap has erased otadata. Seed a VALID selector for
 * the already-healthy running slot before its first OTA, otherwise the first
 * candidate has no previous VALID entry to roll back to. Never do this at init
 * or for an existing candidate selected for a different slot. */
static esp_err_t confirm_bootloader_health(bool allow_bootstrap)
{
    const esp_partition_t *running=esp_ota_get_running_partition();
    if (!running) return ESP_ERR_INVALID_STATE;
    esp_ota_img_states_t state;
    esp_err_t err=esp_ota_get_state_partition(running,&state);
    if (err==ESP_OK) {
        if (state==ESP_OTA_IMG_NEW || state==ESP_OTA_IMG_PENDING_VERIFY) return esp_ota_mark_app_valid_cancel_rollback();
        return state==ESP_OTA_IMG_VALID || state==ESP_OTA_IMG_UNDEFINED ? ESP_OK : ESP_ERR_INVALID_STATE;
    }
    bool ota_slot=running->subtype==ESP_PARTITION_SUBTYPE_APP_OTA_0 || running->subtype==ESP_PARTITION_SUBTYPE_APP_OTA_1;
    if (!ota_slot) return ESP_OK; /* standalone factory image has no OTA state */
    if (err!=ESP_ERR_NOT_FOUND || !allow_bootstrap || esp_ota_get_boot_partition()!=running) return err==ESP_ERR_NOT_FOUND ? ESP_ERR_INVALID_STATE : err;
    err=esp_ota_set_boot_partition(running);
    return err==ESP_OK ? esp_ota_mark_app_valid_cancel_rollback() : err;
}
#if CONFIG_RW_OTA_ENABLED
#include "ota_manifest.h"
#include "ota_journal.h"
#include "auth.h"
#include "power_btn.h"
#include "usb_hid.h"
#include "esp_crt_bundle.h"
#include "esp_flash.h"
#include "esp_http_client.h"
#include "esp_image_format.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "sodium.h"
#include <stdatomic.h>
#include <ctype.h>
#include <stdlib.h>
#include <time.h>
#if CONFIG_SPIRAM
#include "esp_psram.h"
#endif

#define HTTP_REDIRECTS 3
#define HTTP_REDIRECT_URL_MAX 2047
#define DOWNLOAD_BUDGET_US (600LL * 1000000)
#define HTTP_BUDGET_US (40LL * 1000000)
static const char *TAG = "rw.ota";
static rw_ota_config_t s_config;
static rw_ota_journal_t s_journal;
static nvs_handle_t s_nvs;
static char s_journal_key[16];
static SemaphoreHandle_t s_control;
static QueueHandle_t s_queue;
static TaskHandle_t s_task;
static atomic_bool s_initialized, s_enrolled;
static bool s_healthy;
static int64_t s_health_deadline;
static portMUX_TYPE s_status_mux = portMUX_INITIALIZER_UNLOCKED;
static rw_ota_status_t s_status = {.phase = RW_OTA_DISABLED};
typedef struct { char url[RW_OTA_URL_MAX + 1]; } offer_t;

void rw_ota_get_status(rw_ota_status_t *out)
{
    if (!out) return;
    portENTER_CRITICAL(&s_status_mux); *out = s_status; portEXIT_CRITICAL(&s_status_mux);
}
static void publish(rw_ota_phase_t phase, esp_err_t error, const rw_ota_manifest_t *m, uint32_t downloaded)
{
    portENTER_CRITICAL(&s_status_mux);
    s_status.phase = phase; s_status.error = error; s_status.downloaded = downloaded;
    if (m) { s_status.release = m->identity.release; s_status.total = m->size; strcpy(s_status.version, m->identity.version); }
    rw_ota_status_t snapshot = s_status;
    portEXIT_CRITICAL(&s_status_mux);
    if (s_config.progress && xTaskGetCurrentTaskHandle() == s_task) s_config.progress(s_config.ctx, &snapshot);
}
static esp_err_t save_journal(const rw_ota_journal_t *next)
{
    esp_err_t err = nvs_set_blob(s_nvs, s_journal_key, next, sizeof(*next));
    if (err == ESP_OK) err = nvs_commit(s_nvs);
    if (err == ESP_OK) s_journal = *next;
    return err;
}
static void scope_hash(const rw_ota_image_identity_t *id, uint8_t out[32])
{
    crypto_hash_sha256_state hash; crypto_hash_sha256_init(&hash);
    const char *fields[] = {id->product,id->board,id->profile,id->channel,id->layout,id->chip};
    for (size_t i = 0; i < sizeof(fields)/sizeof(fields[0]); ++i) {
        crypto_hash_sha256_update(&hash, (const uint8_t *)fields[i], strlen(fields[i]) + 1);
    }
    crypto_hash_sha256_final(&hash, out);
}
static const esp_partition_t *partition_at(uint32_t address)
{
    for (unsigned n = 0; n < 2; ++n) {
        const esp_partition_t *p = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0 + n, NULL);
        if (p && p->address == address) return p;
    }
    return NULL;
}
static esp_err_t reconcile_boot(void)
{
    if (!s_journal.pending_release) return ESP_OK;
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (running->address == s_journal.pending_address && s_config.image_identity->release == s_journal.pending_release) return ESP_OK;
    const esp_partition_t *candidate = partition_at(s_journal.pending_address);
    esp_ota_img_states_t state;
    rw_ota_journal_t next = s_journal;
    bool rejected = candidate && esp_ota_get_state_partition(candidate, &state) == ESP_OK &&
                    (state == ESP_OTA_IMG_ABORTED || state == ESP_OTA_IMG_INVALID);
    if (rejected) rw_ota_journal_reject(&next, s_journal.pending_hash);
    else rw_ota_journal_clear_pending(&next); /* reset before boot selection: retry is safe */
    esp_err_t err = save_journal(&next);
    if (err == ESP_OK && rejected) publish(RW_OTA_ROLLED_BACK, ESP_FAIL, NULL, 0);
    return err;
}
bool rw_ota_get_registration(rw_ota_registration_t *out)
{
    if (!out || !s_initialized || !s_enrolled) return false;
    const rw_ota_image_identity_t *i = s_config.image_identity;
    *out = (rw_ota_registration_t){.release_counter=i->release,.updater_api=RW_OTA_API_VERSION};
    strcpy(out->product,i->product); strcpy(out->board,i->board); strcpy(out->profile,i->profile);
    strcpy(out->channel,i->channel); strcpy(out->partition_layout,i->layout); strcpy(out->version,i->version);
    return true;
}
esp_err_t rw_ota_offer_manifest(const char *url)
{
    if (!s_initialized || !s_enrolled || s_config.policy == RW_OTA_PINNED) return ESP_ERR_NOT_SUPPORTED;
    if (!url || strnlen(url, RW_OTA_URL_MAX + 1) > RW_OTA_URL_MAX || !rw_ota_url_in_source(url,s_config.source_prefix)) return ESP_ERR_INVALID_ARG;
    rw_ota_status_t status; rw_ota_get_status(&status);
    if (status.phase == RW_OTA_DOWNLOADING || status.phase == RW_OTA_VERIFYING ||
        status.phase == RW_OTA_WAITING_RESTART || status.phase == RW_OTA_RESTARTING || status.phase == RW_OTA_PENDING_HEALTH) return ESP_ERR_INVALID_STATE;
    offer_t offer = {0}; strcpy(offer.url,url);
    return xQueueSend(s_queue,&offer,0) == pdTRUE ? ESP_OK : ESP_ERR_INVALID_STATE;
}
/* Redirects may move to a CDN only when the application explicitly trusts that
 * exact hostname. TLS verification stays enabled on every connection. */
static bool redirect_allowed(const char *url)
{
    char host[128], original[128];
    if (!rw_ota_https_host(url,host,sizeof(host)) || !rw_ota_https_host(s_config.manifest_url,original,sizeof(original))) return false;
    if (!strcmp(host,original)) return true;
    for (size_t n=0; n<s_config.redirect_host_count; ++n) if (!strcmp(host,s_config.redirect_hosts[n])) return true;
    return false;
}
static esp_http_client_handle_t open_https(const char *url, int64_t deadline, int64_t *length)
{
    esp_http_client_config_t config = {
        .url=url, .timeout_ms=10000, .crt_bundle_attach=esp_crt_bundle_attach,
        .disable_auto_redirect=true, .max_redirection_count=HTTP_REDIRECTS,
        .buffer_size=2048, .buffer_size_tx=512, .user_agent="remote-wake-ota/1",
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) return NULL;
    esp_http_client_set_header(client,"Accept-Encoding","identity");
    for (unsigned redirects=0; redirects<=HTTP_REDIRECTS; ++redirects) {
        if (esp_timer_get_time()>=deadline || esp_http_client_open(client,0)!=ESP_OK) break;
        int64_t size=esp_http_client_fetch_headers(client);
        int code=esp_http_client_get_status_code(client);
        if (size<0) break;
        if (code==200) { *length=size; return client; }
        if (!(code==301 || code==302 || code==303 || code==307 || code==308) || redirects==HTTP_REDIRECTS) break;
        if (esp_http_client_set_redirection(client)!=ESP_OK) break;
        char *next=malloc(HTTP_REDIRECT_URL_MAX+1);
        bool allowed=next && esp_http_client_get_url(client,next,HTTP_REDIRECT_URL_MAX+1)==ESP_OK && redirect_allowed(next);
        free(next);
        if (!allowed) break;
        esp_http_client_close(client);
    }
    esp_http_client_cleanup(client); return NULL;
}
static esp_err_t fetch_manifest(const char *url, rw_ota_manifest_t *manifest)
{
    int64_t deadline=esp_timer_get_time()+HTTP_BUDGET_US, length;
    esp_http_client_handle_t client=open_https(url,deadline,&length);
    if (!client) return ESP_FAIL;
    esp_err_t err=ESP_ERR_INVALID_SIZE;
    char *body=NULL;
    if (length>RW_OTA_MANIFEST_MAX) goto done;
    body=malloc(RW_OTA_MANIFEST_MAX+1);
    if (!body) { err=ESP_ERR_NO_MEM; goto done; }
    size_t received=0;
    while (received<RW_OTA_MANIFEST_MAX && esp_timer_get_time()<deadline) {
        int count=esp_http_client_read(client,body+received,RW_OTA_MANIFEST_MAX-received);
        if (count<0) { err=ESP_FAIL; goto done; }
        if (!count) break;
        received+=count;
    }
    if (!esp_http_client_is_complete_data_received(client) || !received ||
        (length>0 && received!=(size_t)length)) goto done;
    body[received]=0;
    uint32_t flash=0, psram=0;
    err=esp_flash_get_size(NULL,&flash); if (err!=ESP_OK) goto done;
#if CONFIG_SPIRAM
    psram=(uint32_t)esp_psram_get_size();
#endif
    err=rw_ota_manifest_verify(&s_config,body,received,s_config.clock_valid(s_config.ctx),time(NULL),
                               flash,psram,0,manifest);
    /* Manifest parsing does not hold the NVS/control mutex. Recheck the
     * durable floor and suppression under that mutex before any flash write. */
    if (err==ESP_OK) {
        xSemaphoreTake(s_control,portMAX_DELAY);
        if (manifest->identity.release<=s_journal.floor || rw_ota_journal_rejected(&s_journal,manifest->sha256)) err=ESP_ERR_INVALID_STATE;
        xSemaphoreGive(s_control);
    }
done:
    free(body); esp_http_client_cleanup(client); return err;
}
/* Only scan loadable segments, never arbitrary appended bytes. Keep 235 bytes
 * between reads so a metadata record crossing a network/flash block is found. */
static esp_err_t validate_identity(const esp_partition_t *part, const rw_ota_manifest_t *manifest)
{
    esp_image_header_t header;
    esp_err_t err=esp_partition_read(part,0,&header,sizeof(header));
    if (err!=ESP_OK) return err;
    if (header.magic!=ESP_IMAGE_HEADER_MAGIC || !header.segment_count || header.segment_count>16) return ESP_ERR_INVALID_ARG;
    uint8_t *buffer=malloc(4096+sizeof(rw_ota_image_identity_t)-1);
    if (!buffer) return ESP_ERR_NO_MEM;
    size_t position=sizeof(header); unsigned records=0;
    for (unsigned segment=0; segment<header.segment_count; ++segment) {
        esp_image_segment_header_t sh;
        if (position>manifest->size || manifest->size-position<sizeof(sh) ||
            esp_partition_read(part,position,&sh,sizeof(sh))!=ESP_OK) { err=ESP_ERR_INVALID_SIZE; goto done; }
        position+=sizeof(sh);
        if (sh.data_len>manifest->size-position) { err=ESP_ERR_INVALID_SIZE; goto done; }
        size_t offset=0, tail=0;
        while (offset<sh.data_len) {
            size_t count=sh.data_len-offset; if (count>4096) count=4096;
            err=esp_partition_read(part,position+offset,buffer+tail,count); if (err!=ESP_OK) goto done;
            size_t available=tail+count;
            for (size_t p=0; p+sizeof(rw_ota_image_identity_t)<=available; ++p) {
                if (buffer[p]!='R' || memcmp(buffer+p,RW_OTA_IDENTITY_MAGIC,16)) continue;
                rw_ota_image_identity_t id; memcpy(&id,buffer+p,sizeof(id));
                if (id.format_version!=1 || id.updater_api!=RW_OTA_API_VERSION) continue;
                ++records;
                if (!rw_ota_identity_valid(&id) || !rw_ota_identity_tuple_equal(&id,&manifest->identity) ||
                    id.release!=manifest->identity.release || strcmp(id.version,manifest->identity.version)) { err=ESP_ERR_NOT_SUPPORTED; goto done; }
            }
            tail=available<sizeof(rw_ota_image_identity_t)-1 ? available : sizeof(rw_ota_image_identity_t)-1;
            memmove(buffer,buffer+available-tail,tail); offset+=count;
        }
        position+=sh.data_len;
    }
    esp_app_desc_t app;
    err=records==1 ? esp_ota_get_partition_description(part,&app) : ESP_ERR_INVALID_ARG;
    if (err==ESP_OK && (app.magic_word!=ESP_APP_DESC_MAGIC_WORD ||
        strnlen(app.version,sizeof(app.version))==sizeof(app.version) || strcmp(app.version,manifest->identity.version))) err=ESP_ERR_NOT_SUPPORTED;
done:
    free(buffer); return err;
}
static esp_err_t download_image(const rw_ota_manifest_t *manifest, const esp_partition_t **installed, bool *rejected)
{
    *rejected=false;
    const esp_partition_t *part=esp_ota_get_next_update_partition(NULL), *running=esp_ota_get_running_partition();
    if (!part || part==running || manifest->size>part->size || manifest->size<sizeof(esp_image_header_t)) return ESP_ERR_INVALID_SIZE;
    int64_t length, deadline=esp_timer_get_time()+DOWNLOAD_BUDGET_US;
    esp_http_client_handle_t client=open_https(manifest->url,deadline,&length);
    if (!client) return ESP_FAIL;
    esp_ota_handle_t handle=0; bool begun=false;
    esp_err_t err=ESP_ERR_INVALID_SIZE;
    uint8_t *buffer=NULL;
    if (length>0 && length!=manifest->size) goto done;
    buffer=malloc(4096); if (!buffer) { err=ESP_ERR_NO_MEM; goto done; }
    err=esp_ota_begin(part,OTA_WITH_SEQUENTIAL_WRITES,&handle); if (err!=ESP_OK) goto done;
    begun=true;
    crypto_hash_sha256_state hash; crypto_hash_sha256_init(&hash);
    uint32_t received=0, last_progress=0;
    while (received<manifest->size && esp_timer_get_time()<deadline) {
        size_t wanted=manifest->size-received; if (wanted>4096) wanted=4096;
        int count=esp_http_client_read(client,(char *)buffer,wanted);
        if (count<=0) { err=count<0 ? ESP_FAIL : ESP_ERR_INVALID_SIZE; goto done; }
        crypto_hash_sha256_update(&hash,buffer,count);
        err=esp_ota_write(handle,buffer,count); if (err!=ESP_OK) goto done;
        received+=count;
        if (received-last_progress>=65536 || received==manifest->size) { publish(RW_OTA_DOWNLOADING,ESP_OK,manifest,received); last_progress=received; }
        vTaskDelay(1);
    }
    if (received!=manifest->size) { err=ESP_ERR_TIMEOUT; goto done; }
    /* Reject a chunked response with hidden bytes past the declared image. */
    char extra; int more=esp_http_client_read(client,&extra,1);
    if (more!=0 || !esp_http_client_is_complete_data_received(client)) { err=ESP_ERR_INVALID_SIZE; goto done; }
    uint8_t digest[32]; crypto_hash_sha256_final(&hash,digest);
    if (sodium_memcmp(digest,manifest->sha256,32)) { err=ESP_ERR_INVALID_CRC; goto done; }
    publish(RW_OTA_VERIFYING,ESP_OK,manifest,received);
    err=esp_ota_end(handle); begun=false;
    if (err!=ESP_OK) { *rejected=err==ESP_ERR_OTA_VALIDATE_FAILED; goto done; }
    err=validate_identity(part,manifest);
    if (err!=ESP_OK) { *rejected=err==ESP_ERR_NOT_SUPPORTED || err==ESP_ERR_INVALID_ARG || err==ESP_ERR_INVALID_SIZE; goto done; }
    *installed=part;
done:
    if (begun) esp_ota_abort(handle);
    free(buffer); esp_http_client_cleanup(client); return err;
}
static void cancel_reservations(void)
{
    rw_hid_resume_actions(); rw_power_btn_resume_actions();
}
static esp_err_t restart_candidate(const rw_ota_manifest_t *m, const esp_partition_t *part)
{
    publish(RW_OTA_WAITING_RESTART,ESP_OK,m,m->size);
    int64_t deadline=esp_timer_get_time()+60LL*1000000;
    while (esp_timer_get_time()<deadline) {
        esp_err_t err=rw_power_btn_quiesce();
        if (err==ESP_OK) {
            err=rw_hid_quiesce();
            if (err!=ESP_OK) rw_power_btn_resume_actions();
            else {
                err=s_config.prepare_restart ? s_config.prepare_restart(s_config.ctx) : ESP_OK;
                if (err==ESP_OK) {
                    xSemaphoreTake(s_control,portMAX_DELAY);
                    rw_ota_journal_t next=s_journal;
                    rw_ota_journal_prepare(&next,m->identity.release,part->address,m->sha256);
                    err=save_journal(&next);
                    if (err==ESP_OK) err=esp_ota_set_boot_partition(part);
                    xSemaphoreGive(s_control);
                    if (err==ESP_OK) {
                        publish(RW_OTA_RESTARTING,ESP_OK,m,m->size);
                        vTaskDelay(pdMS_TO_TICKS(200)); esp_restart();
                    }
                    cancel_reservations(); return err;
                }
                cancel_reservations();
            }
        }
        if (err!=ESP_ERR_INVALID_STATE && err!=ESP_ERR_TIMEOUT) return err;
        vTaskDelay(pdMS_TO_TICKS(250));
    }
    return ESP_ERR_TIMEOUT;
}
esp_err_t rw_ota_confirm_healthy(void)
{
    if (!s_initialized) return confirm_bootloader_health(true);
    xSemaphoreTake(s_control,portMAX_DELAY);
    /* Idempotent health reports must not erase an installer's pending journal
     * during the short interval after boot selection but before restart. */
    if (s_healthy) { xSemaphoreGive(s_control); return ESP_OK; }
    esp_err_t err=confirm_bootloader_health(!s_journal.pending_release);
    if (err==ESP_OK) {
        rw_ota_journal_t next=s_journal; rw_ota_journal_confirm(&next,s_config.image_identity->release);
        err=save_journal(&next);
        if (err==ESP_OK) { s_healthy=true; publish(RW_OTA_HEALTHY,ESP_OK,NULL,0); }
    }
    xSemaphoreGive(s_control); return err;
}
esp_err_t rw_ota_reject_unhealthy(void)
{
    if (!boot_pending()) return ESP_ERR_INVALID_STATE;
    if (s_initialized) {
        xSemaphoreTake(s_control,portMAX_DELAY);
        rw_ota_journal_t next=s_journal;
        if (next.pending_release) rw_ota_journal_reject(&next,s_journal.pending_hash);
        esp_err_t saved=save_journal(&next);
        xSemaphoreGive(s_control);
        if (saved!=ESP_OK) ESP_LOGE(TAG,"failed to persist rollback suppression");
    }
    return esp_ota_mark_app_invalid_rollback_and_reboot();
}
static void worker(void *arg)
{
    (void)arg;
    /* s_task is assigned by xTaskCreate before initialization is published. */
    while (!s_initialized) vTaskDelay(1);
    int64_t next_check=0, earliest_hint=0;
    uint32_t failures=0;
    offer_t deferred={0}; bool has_deferred=false;
    for (;;) {
        offer_t offer;
        if (xQueueReceive(s_queue,&offer,pdMS_TO_TICKS(1000))==pdTRUE) { deferred=offer; has_deferred=true; }
        int64_t now=esp_timer_get_time();
        xSemaphoreTake(s_control,portMAX_DELAY);
        bool healthy=s_healthy;
        bool expired=!healthy && now>=s_health_deadline && boot_pending();
        xSemaphoreGive(s_control);
        if (expired) { publish(RW_OTA_FAILED,ESP_ERR_TIMEOUT,NULL,0); rw_ota_reject_unhealthy(); continue; }
        if (!healthy || !s_enrolled || s_config.policy==RW_OTA_PINNED) continue;
        if ((has_deferred && now<earliest_hint) || (!has_deferred && now<next_check)) continue;
        const char *url=has_deferred ? deferred.url : s_config.manifest_url;
        has_deferred=false;
        publish(RW_OTA_CHECKING,ESP_OK,NULL,0);
        rw_ota_manifest_t *m=calloc(1,sizeof(*m));
        esp_err_t err=m ? fetch_manifest(url,m) : ESP_ERR_NO_MEM;
        if (err==ESP_OK) {
            publish(RW_OTA_AVAILABLE,ESP_OK,m,0);
            if (s_config.policy==RW_OTA_AUTO) {
                const esp_partition_t *part=NULL;
                publish(RW_OTA_DOWNLOADING,ESP_OK,m,0);
                bool rejected=false;
                err=download_image(m,&part,&rejected);
                if (rejected) {
                    xSemaphoreTake(s_control,portMAX_DELAY);
                    rw_ota_journal_t next=s_journal; rw_ota_journal_reject(&next,m->sha256);
                    esp_err_t saved=save_journal(&next);
                    xSemaphoreGive(s_control);
                    if (saved!=ESP_OK) err=saved;
                }
                if (err==ESP_OK) err=restart_candidate(m,part);
            }
        }
        if (err!=ESP_OK) { publish(RW_OTA_FAILED,err,m,0); if (failures<10) ++failures; }
        else failures=0;
        free(m);
        uint32_t interval=s_config.check_interval_sec ? s_config.check_interval_sec : 21600;
        uint32_t retry=failures ? (30U << (failures>8 ? 8 : failures-1)) : interval;
        if (retry>interval) retry=interval;
        uint32_t jitter=esp_random() % (retry/5+1);
        next_check=esp_timer_get_time()+((int64_t)retry+jitter)*1000000;
        earliest_hint=esp_timer_get_time()+30LL*1000000;
    }
}
esp_err_t rw_ota_init(const rw_ota_config_t *config)
{
    if (s_initialized) return ESP_ERR_INVALID_STATE;
#if !CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
    (void)config; return ESP_ERR_NOT_SUPPORTED;
#else
    if (!config || !rw_ota_identity_valid(config->image_identity) || strcmp(config->image_identity->chip,CONFIG_IDF_TARGET) ||
        strcmp(config->image_identity->version,esp_app_get_description()->version) || config->policy<RW_OTA_NOTIFY || config->policy>RW_OTA_PINNED ||
        config->trusted_key_count>RW_OTA_MAX_KEYS || config->redirect_host_count>4 ||
        (config->check_interval_sec && config->check_interval_sec<300) ||
        config->check_interval_sec>604800 || config->health_timeout_sec>600) return ESP_ERR_INVALID_ARG;
    bool enrolled=config->trusted_key_count>0;
    if (enrolled && (!config->trusted_keys || !config->clock_valid ||
        !rw_ota_url_in_source(config->manifest_url,config->source_prefix))) return ESP_ERR_INVALID_ARG;
    for (size_t n=0; n<config->trusted_key_count; ++n) {
        const rw_ota_trusted_key_t *k=&config->trusted_keys[n];
        if (!k->generation || !k->kid[0] || strnlen(k->kid,sizeof(k->kid))==sizeof(k->kid)) return ESP_ERR_INVALID_ARG;
        for (size_t j=0; j<n; ++j) if (!strcmp(k->kid,config->trusted_keys[j].kid)) return ESP_ERR_INVALID_ARG;
    }
    if (config->redirect_host_count && !config->redirect_hosts) return ESP_ERR_INVALID_ARG;
    for (size_t n=0; n<config->redirect_host_count; ++n) {
        const char *host=config->redirect_hosts[n];
        if (!host || !*host || strlen(host)>127) return ESP_ERR_INVALID_ARG;
        for (const char *p=host; *p; ++p) if (!(isalnum((unsigned char)*p)||*p=='.'||*p=='-')) return ESP_ERR_INVALID_ARG;
    }
    const esp_partition_t *a=esp_partition_find_first(ESP_PARTITION_TYPE_APP,ESP_PARTITION_SUBTYPE_APP_OTA_0,NULL);
    const esp_partition_t *b=esp_partition_find_first(ESP_PARTITION_TYPE_APP,ESP_PARTITION_SUBTYPE_APP_OTA_1,NULL);
    const esp_partition_t *data=esp_partition_find_first(ESP_PARTITION_TYPE_DATA,ESP_PARTITION_SUBTYPE_DATA_OTA,NULL);
    if (!a || !b || !data || data->size<8192) return ESP_ERR_NOT_SUPPORTED;
    esp_err_t err=rw_auth_init(); if (err!=ESP_OK) return err;
    err=nvs_open("rw_ota",NVS_READWRITE,&s_nvs); if (err!=ESP_OK) return err;
    uint8_t scope[32]; scope_hash(config->image_identity,scope);
    s_journal_key[0]='j';
    for (unsigned n=0; n<7; ++n) {
        static const char hex[]="0123456789abcdef";
        s_journal_key[1+2*n]=hex[scope[n]>>4]; s_journal_key[2+2*n]=hex[scope[n]&15];
    }
    s_journal_key[15]=0;
    size_t bytes=sizeof(s_journal);
    err=nvs_get_blob(s_nvs,s_journal_key,&s_journal,&bytes);
    if (err==ESP_ERR_NVS_NOT_FOUND) {
        s_journal=(rw_ota_journal_t){.format=RW_OTA_JOURNAL_VERSION}; memcpy(s_journal.scope,scope,32); err=ESP_OK;
        bytes=sizeof(s_journal);
    }
    if (err!=ESP_OK || bytes!=sizeof(s_journal) || !rw_ota_journal_valid(&s_journal,scope)) { nvs_close(s_nvs); return err==ESP_OK ? ESP_ERR_INVALID_STATE : err; }
    s_config=*config; s_enrolled=enrolled;
    portENTER_CRITICAL(&s_status_mux);
    s_status=(rw_ota_status_t){.phase=RW_OTA_IDLE,.release=config->image_identity->release};
    strcpy(s_status.version,config->image_identity->version);
    portEXIT_CRITICAL(&s_status_mux);
    s_control=xSemaphoreCreateMutex();
    s_queue=xQueueCreate(1,sizeof(offer_t));
    if (!s_control || !s_queue) { err=ESP_ERR_NO_MEM; goto failed; }
    s_healthy=false;
    s_health_deadline=esp_timer_get_time()+(int64_t)(config->health_timeout_sec ? config->health_timeout_sec : 60)*1000000;
    publish(boot_pending() ? RW_OTA_PENDING_HEALTH : RW_OTA_IDLE,ESP_OK,NULL,0);
    err=reconcile_boot(); if (err!=ESP_OK) goto failed;
    if (xTaskCreate(worker,"rw-ota",8192,NULL,3,&s_task)!=pdPASS) { err=ESP_ERR_NO_MEM; goto failed; }
    s_initialized=true; return ESP_OK;
failed:
    if (s_control) { vSemaphoreDelete(s_control); s_control=NULL; }
    if (s_queue) { vQueueDelete(s_queue); s_queue=NULL; }
    s_enrolled=false; nvs_close(s_nvs); return err;
#endif
}
#else
esp_err_t rw_ota_init(const rw_ota_config_t *config) { (void)config; return ESP_ERR_NOT_SUPPORTED; }
bool rw_ota_get_registration(rw_ota_registration_t *out) { (void)out; return false; }
esp_err_t rw_ota_offer_manifest(const char *url) { (void)url; return ESP_ERR_NOT_SUPPORTED; }
void rw_ota_get_status(rw_ota_status_t *out) { if (out) *out=(rw_ota_status_t){.phase=RW_OTA_DISABLED}; }
esp_err_t rw_ota_confirm_healthy(void) { return confirm_bootloader_health(true); }
esp_err_t rw_ota_reject_unhealthy(void) { return boot_pending() ? esp_ota_mark_app_invalid_rollback_and_reboot() : ESP_ERR_INVALID_STATE; }
#endif
