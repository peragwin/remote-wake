/* Production updater with real JSON/Ed25519/SHA256 and mocked flash, HTTP, NVS
 * and bootloader boundaries. No second implementation of the updater. */
#include "ota_mock.h"
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <time.h>
static time_t test_time(time_t *out) { if(out)*out=1750000000;return 1750000000; }
#define time test_time
#include "../../main/ota.c"
#undef time

static esp_partition_t parts[3]={{.address=0x10000,.size=16384,.type=0,.subtype=0x10},{.address=0x1f0000,.size=16384,.type=0,.subtype=0x11},{.address=0x3d0000,.size=8192,.type=1,.subtype=0}};
static esp_ota_img_states_t states[2];
static unsigned running,boot_selected,begin_count,abort_count,end_count,setboot_count,markvalid_count,rollback_count;
static unsigned write_fail_after,flash_used,read_limit,progress_failures,stop_failures,app_busy;
static uint8_t flash_bytes[16384],image[16384],original_image[16384],durable[1024],staged[1024];
static size_t image_size,original_size,durable_size,staged_size;
static char *manifest_json;
static bool storage_fail,selector_fail,write_error,read_error,power_busy,hid_busy,power_reserved,hid_reserved,task_fail,queue_full,in_worker;
static bool redirect_on,redirect_bad,tls_fail,missing_selector,power_cut_after_write,invalid_image_end,test_deferred,stop_available;
static char last_url[2048];
static offer_t queued_offer;
static unsigned opened,cleaned;
static int64_t now_us,read_delay;
static int64_t header_size;
static jmp_buf stop;
static rw_ota_image_identity_t current={.magic=RW_OTA_IDENTITY_MAGIC_BYTES,.format_version=1,.updater_api=1,.release=6,
    .product="remote-wake",.board="esp32-s3-devkitc",.profile="hid-power",.channel="stable",.layout="rw-4m-ab-v1",.version="1.2.2",.chip="esp32s3"};
static rw_ota_trusted_key_t publisher={.kid="test-rfc8032",.generation=1};
static const char *hosts[]={"release-assets.githubusercontent.com"};
static rw_ota_config_t config;
static esp_app_desc_t current_desc;
struct http_mock { char url[2048];const uint8_t *body;size_t size,offset; };
static bool clock_ok(void *ctx) { (void)ctx;return true; }
static esp_err_t prepare(void *ctx) { (void)ctx;if(app_busy){--app_busy;return ESP_ERR_INVALID_STATE;}return ESP_OK; }
static void progress(void *ctx,const rw_ota_status_t *s)
{ (void)ctx;
    if (stop_available && s->phase==RW_OTA_AVAILABLE) longjmp(stop,2);
    if (s->phase==RW_OTA_FAILED) {
        ++progress_failures;
        if(test_deferred && progress_failures==1) assert(rw_ota_offer_manifest("https://github.com/example/remote-wake/releases/newer-manifest.json")==ESP_OK);
        if(stop_failures && progress_failures>=stop_failures)longjmp(stop,2);
    }
}
static void reset_engine(bool keep_storage)
{
    s_initialized=s_enrolled=false;s_task=NULL;s_control=NULL;s_queue=NULL;s_healthy=false;
    s_status=(rw_ota_status_t){.phase=RW_OTA_DISABLED};
    current.release=6;strcpy(current.version,"1.2.2");current_desc=(esp_app_desc_t){.magic_word=ESP_APP_DESC_MAGIC_WORD};strcpy(current_desc.version,current.version);
    config=(rw_ota_config_t){.image_identity=&current,.trusted_keys=&publisher,.trusted_key_count=1,
      .manifest_url="https://github.com/example/remote-wake/releases/manifest.json",.source_prefix="https://github.com/example/remote-wake/releases/",
      .redirect_hosts=hosts,.redirect_host_count=1,.policy=RW_OTA_AUTO,.clock_valid=clock_ok,.prepare_restart=prepare,.progress=progress};
    running=boot_selected=0;states[0]=ESP_OTA_IMG_VALID;states[1]=ESP_OTA_IMG_UNDEFINED;
    begin_count=abort_count=end_count=setboot_count=markvalid_count=rollback_count=0;
    write_fail_after=flash_used=progress_failures=stop_failures=app_busy=0;
    storage_fail=selector_fail=write_error=read_error=power_busy=hid_busy=power_reserved=hid_reserved=task_fail=queue_full=in_worker=false;
    redirect_on=redirect_bad=tls_fail=missing_selector=power_cut_after_write=invalid_image_end=test_deferred=stop_available=false;opened=cleaned=0;last_url[0]=0;
    image_size=original_size;memcpy(image,original_image,image_size);memset(flash_bytes,0,sizeof(flash_bytes));
    now_us=0;read_delay=1000;read_limit=127;header_size=-2;
    if(!keep_storage){durable_size=staged_size=0;memset(durable,0,sizeof(durable));}
}
static rw_ota_manifest_t parsed(void)
{
    rw_ota_manifest_t m;assert(rw_ota_manifest_verify(&config,manifest_json,strlen(manifest_json),true,1750000000,4194304,0,6,&m)==ESP_OK);return m;
}
static void start_healthy(void)
{
    assert(rw_ota_init(&config)==ESP_OK);assert(rw_ota_confirm_healthy()==ESP_OK);assert(s_journal.floor==6);
}
static void test_truncated_journal(void)
{
    reset_engine(false); start_healthy();
    /* A prefix can still have valid format/scope and stale RAM fields. Never
     * let a short NVS blob silently weaken the durable release history. */
    durable_size = 36;
    reset_engine(true);
    assert(rw_ota_init(&config) == ESP_ERR_INVALID_STATE);
    assert(!s_initialized && !opened && !setboot_count);
}
static void test_download(void)
{
    reset_engine(false);start_healthy();rw_ota_manifest_t m=parsed();const esp_partition_t *installed=NULL;bool rejected;
    assert(download_image(&m,&installed,&rejected)==ESP_OK && installed==&parts[1] && !rejected);
    assert(begin_count==1 && end_count==1 && abort_count==0 && !setboot_count);
    assert(flash_used==m.size && !memcmp(flash_bytes,image,image_size) && opened==cleaned);
    reset_engine(false);start_healthy();m=parsed();image_size-=3;header_size=m.size;
    assert(download_image(&m,&installed,&rejected)!=ESP_OK && abort_count==1 && !setboot_count);
    reset_engine(false);start_healthy();m=parsed();image[100]^=1;
    assert(download_image(&m,&installed,&rejected)==ESP_ERR_INVALID_CRC && abort_count==1 && !rejected && !setboot_count);
    reset_engine(false);start_healthy();m=parsed();read_error=true;
    assert(download_image(&m,&installed,&rejected)!=ESP_OK && abort_count==1 && flash_used>0 && !setboot_count);
    reset_engine(false);start_healthy();m=parsed();write_error=true;
    assert(download_image(&m,&installed,&rejected)!=ESP_OK && abort_count==1 && flash_used>0 && !setboot_count);
    reset_engine(false);start_healthy();m=parsed();header_size=m.size+1;
    assert(download_image(&m,&installed,&rejected)==ESP_ERR_INVALID_SIZE && !begin_count);
    reset_engine(false);start_healthy();m=parsed();m.size=parts[1].size+1;
    assert(download_image(&m,&installed,&rejected)==ESP_ERR_INVALID_SIZE && !begin_count && !opened);
    /* A fully authenticated image with mismatched embedded profile is rejected
     * permanently. The signature authenticates the manifest hash before this. */
    reset_engine(false);start_healthy();m=parsed();image[288+92]='X';crypto_hash_sha256(m.sha256,image,image_size);
    assert(download_image(&m,&installed,&rejected)==ESP_ERR_NOT_SUPPORTED && rejected && !setboot_count);
    /* Split the identity across4096-byte flash reads, retaining only235 bytes. */
    reset_engine(false);start_healthy();m=parsed();memmove(image+288+3800,image+288,image_size-288);memset(image+288,0,3800);image_size+=3800;
    uint32_t segment_size;memcpy(&segment_size,image+28,4);segment_size+=3800;memcpy(image+28,&segment_size,4);
    m.size=image_size;crypto_hash_sha256(m.sha256,image,image_size);
    assert(download_image(&m,&installed,&rejected)==ESP_OK && !rejected);
    reset_engine(false);start_healthy();m=parsed();invalid_image_end=true;
    assert(download_image(&m,&installed,&rejected)==ESP_ERR_OTA_VALIDATE_FAILED && rejected && !setboot_count);
    reset_engine(false);start_healthy();m=parsed();power_cut_after_write=true;
    if(!setjmp(stop)){download_image(&m,&installed,&rejected);assert(false);}
    assert(flash_used>0 && !setboot_count && s_journal.floor==6 && !s_journal.pending_release);
    reset_engine(true);assert(rw_ota_init(&config)==ESP_OK && !s_journal.pending_release && s_journal.floor==6);
    reset_engine(false);start_healthy();m=parsed();read_delay=DOWNLOAD_BUDGET_US;
    assert(download_image(&m,&installed,&rejected)==ESP_ERR_TIMEOUT && abort_count==1 && !setboot_count);
}
static void test_redirects(void)
{
    reset_engine(false);start_healthy();rw_ota_manifest_t m=parsed();const esp_partition_t *part=NULL;bool rejected;
    redirect_on=true;assert(download_image(&m,&part,&rejected)==ESP_OK);
    reset_engine(false);start_healthy();m=parsed();redirect_on=redirect_bad=true;
    assert(download_image(&m,&part,&rejected)!=ESP_OK && !begin_count && opened==cleaned);
    reset_engine(false);start_healthy();m=parsed();tls_fail=true;
    assert(download_image(&m,&part,&rejected)!=ESP_OK && !begin_count);
}
static void test_restart_transaction(void)
{
    reset_engine(false);start_healthy();rw_ota_manifest_t m=parsed();storage_fail=true;
    assert(restart_candidate(&m,&parts[1])!=ESP_OK && !setboot_count && !power_reserved && !hid_reserved);
    reset_engine(false);start_healthy();m=parsed();selector_fail=true;
    assert(restart_candidate(&m,&parts[1])!=ESP_OK && setboot_count==1 && !power_reserved && !hid_reserved && s_journal.floor==6);
    /* Power loss after durable candidate, before successful boot selection:
     * reconcile clears prepared state without blacklisting an untried image. */
    s_journal=(rw_ota_journal_t){0};memcpy(&s_journal,durable,durable_size);assert(reconcile_boot()==ESP_OK);
    assert(!s_journal.pending_release && !rw_ota_journal_rejected(&s_journal,m.sha256));
    reset_engine(false);start_healthy();m=parsed();app_busy=1000;
    assert(restart_candidate(&m,&parts[1])==ESP_ERR_TIMEOUT && now_us>=60000000 && !setboot_count && !power_reserved && !hid_reserved);
    reset_engine(false);start_healthy();m=parsed();power_busy=true;
    assert(restart_candidate(&m,&parts[1])==ESP_ERR_TIMEOUT && !setboot_count);
    reset_engine(false);start_healthy();m=parsed();app_busy=2;
    if(!setjmp(stop)){restart_candidate(&m,&parts[1]);assert(false);}
    assert(setboot_count==1 && boot_selected==1 && s_journal.pending_release==7 && s_journal.floor==6 && now_us>=700000);
    /* Simulate first boot failing before health init: rollback app reconciles
     * IDF's aborted slot and persists unchanged-candidate suppression. */
    assert(rw_ota_confirm_healthy()==ESP_OK && s_journal.pending_release==7);
    states[1]=ESP_OTA_IMG_ABORTED;assert(reconcile_boot()==ESP_OK);
    assert(rw_ota_journal_rejected(&s_journal,m.sha256) && s_journal.floor==6);
}
static void test_boot_health(void)
{
    reset_engine(false);start_healthy();rw_ota_manifest_t m=parsed();rw_ota_journal_t j=s_journal;
    rw_ota_journal_prepare(&j,7,parts[1].address,m.sha256);assert(save_journal(&j)==ESP_OK);
    reset_engine(true);running=1;states[1]=ESP_OTA_IMG_PENDING_VERIFY;current.release=7;strcpy(current.version,"1.2.3");strcpy(current_desc.version,current.version);
    config.trusted_key_count=0;config.trusted_keys=NULL;assert(rw_ota_init(&config)==ESP_OK);assert(!s_enrolled);
    /* Commit failure after mark-valid cannot lower the effective release floor
     * on restart, because the embedded running release is always checked. */
    storage_fail=true;assert(rw_ota_confirm_healthy()!=ESP_OK && states[1]==ESP_OTA_IMG_VALID && markvalid_count==1);
    rw_ota_manifest_t out;rw_ota_config_t verifier=config;verifier.trusted_keys=&publisher;verifier.trusted_key_count=1;
    assert(rw_ota_manifest_verify(&verifier,manifest_json,strlen(manifest_json),true,1750000000,4194304,0,6,&out)==ESP_ERR_INVALID_STATE);
    storage_fail=false;assert(rw_ota_confirm_healthy()==ESP_OK && s_journal.floor==7 && !s_journal.pending_release);
    /* Trust-empty enrollment still enforces a pending application's health
     * deadline and durable suppression before IDF rolls it back. */
    reset_engine(false);start_healthy();m=parsed();j=s_journal;rw_ota_journal_prepare(&j,7,parts[1].address,m.sha256);assert(save_journal(&j)==ESP_OK);
    reset_engine(true);running=1;states[1]=ESP_OTA_IMG_PENDING_VERIFY;current.release=7;strcpy(current.version,"1.2.3");strcpy(current_desc.version,current.version);
    config.trusted_key_count=0;config.trusted_keys=NULL;config.health_timeout_sec=2;assert(rw_ota_init(&config)==ESP_OK);
    in_worker=true;if(!setjmp(stop)){worker(NULL);assert(false);}in_worker=false;
    assert(rollback_count==1 && now_us>=2000000 && rw_ota_journal_rejected(&s_journal,m.sha256) && !opened);
}
static void test_factoryless_bootstrap(void)
{
    reset_engine(false);missing_selector=true;
    assert(rw_ota_init(&config)==ESP_OK);
    assert(setboot_count==0 && markvalid_count==0); /* no confirmation at init */
    assert(rw_ota_confirm_healthy()==ESP_OK);
    assert(setboot_count==1 && markvalid_count==1 && states[0]==ESP_OTA_IMG_VALID && s_journal.floor==6);
    reset_engine(false);missing_selector=true;selector_fail=true;
    assert(rw_ota_init(&config)==ESP_OK);
    assert(rw_ota_confirm_healthy()==ESP_FAIL && !markvalid_count && s_journal.floor==0);
    /* Power loss after seed creates NEW: healthy reboot can confirm that NEW
     * entry directly, giving the first candidate a prior VALID selector. */
    reset_engine(false);states[0]=ESP_OTA_IMG_NEW;
    assert(rw_ota_init(&config)==ESP_OK && rw_ota_confirm_healthy()==ESP_OK);
    assert(!setboot_count && markvalid_count==1 && states[0]==ESP_OTA_IMG_VALID);
    reset_engine(false);missing_selector=true;parts[0].subtype=0;
    assert(confirm_bootloader_health(true)==ESP_OK && !setboot_count && !markvalid_count);
    parts[0].subtype=ESP_PARTITION_SUBTYPE_APP_OTA_0;
}
static void test_full_worker(void)
{
    reset_engine(false);start_healthy();in_worker=true;
    if(!setjmp(stop)){worker(NULL);assert(false);}in_worker=false;
    assert(setboot_count==1 && boot_selected==1 && flash_used==image_size);
    reset_engine(false);start_healthy();image[100]^=1;stop_failures=1;in_worker=true;
    if(!setjmp(stop)){worker(NULL);assert(false);}in_worker=false;
    assert(!setboot_count && progress_failures==1 && abort_count==1 && !power_reserved);
    reset_engine(false);start_healthy();invalid_image_end=true;stop_failures=1;in_worker=true;
    if(!setjmp(stop)){worker(NULL);assert(false);}in_worker=false;
    rw_ota_manifest_t rejected=parsed();assert(rw_ota_journal_rejected(&s_journal,rejected.sha256));
    assert(fetch_manifest(config.manifest_url,&rejected)==ESP_ERR_INVALID_STATE && !setboot_count);
    reset_engine(false);start_healthy();tls_fail=true;test_deferred=true;stop_failures=2;in_worker=true;
    if(!setjmp(stop)){worker(NULL);assert(false);}in_worker=false;
    assert(strstr(last_url,"newer-manifest.json") && now_us>=30000000); /* admitted hint survives throttle */
    reset_engine(false);config.policy=RW_OTA_NOTIFY;start_healthy();stop_available=true;in_worker=true;
    if(!setjmp(stop)){worker(NULL);assert(false);}in_worker=false;
    assert(!begin_count && !setboot_count);
    reset_engine(false);start_healthy();task_fail=true; /* initialized APIs remain bounded */
    assert(rw_ota_offer_manifest("https://evil.test/manifest.json")==ESP_ERR_INVALID_ARG);
    assert(rw_ota_offer_manifest(config.manifest_url)==ESP_OK);
    assert(rw_ota_offer_manifest(config.manifest_url)==ESP_ERR_INVALID_STATE);
    reset_engine(false);task_fail=true;assert(rw_ota_init(&config)==ESP_ERR_NO_MEM && !s_initialized && !s_enrolled);
}
int main(int argc,char **argv)
{
    assert(argc==2);FILE *f=fopen(argv[1],"rb");assert(f);fseek(f,0,SEEK_END);long n=ftell(f);rewind(f);char *body=calloc(1,n+1);assert(fread(body,1,n,f)==(size_t)n);fclose(f);
    cJSON *fixture=cJSON_Parse(body);assert(fixture);cJSON *envelope=cJSON_GetObjectItemCaseSensitive(fixture,"envelope");manifest_json=cJSON_PrintUnformatted(envelope);
    const char *hex=cJSON_GetObjectItemCaseSensitive(fixture,"image_hex")->valuestring;original_size=strlen(hex)/2;
    assert(sodium_hex2bin(original_image,sizeof(original_image),hex,strlen(hex),NULL,NULL,NULL)==0);
    hex=cJSON_GetObjectItemCaseSensitive(fixture,"public_key_hex")->valuestring;assert(sodium_hex2bin(publisher.public_key,32,hex,64,NULL,NULL,NULL)==0);
    test_truncated_journal();test_download();test_redirects();test_restart_transaction();test_boot_health();test_factoryless_bootstrap();test_full_worker();
    free(manifest_json);cJSON_Delete(fixture);free(body);puts("OTA production engine flash/HTTPS/health/rollback fault-injection tests passed");
}
SemaphoreHandle_t xSemaphoreCreateMutex(void){return (void*)1;}
int xSemaphoreTake(SemaphoreHandle_t m,TickType_t t){(void)t;assert(m);return pdTRUE;}
void xSemaphoreGive(SemaphoreHandle_t m){assert(m);}
void vSemaphoreDelete(SemaphoreHandle_t m){(void)m;}
QueueHandle_t xQueueCreate(unsigned n,size_t size){assert(n==1 && size==sizeof(offer_t));return (void*)2;}
void vQueueDelete(QueueHandle_t q){(void)q;}
int xQueueSend(QueueHandle_t q,const void *data,TickType_t t){(void)q;(void)t;if(queue_full)return 0;memcpy(&queued_offer,data,sizeof(queued_offer));queue_full=true;return pdTRUE;}
int xQueueReceive(QueueHandle_t q,void *data,TickType_t t){(void)q;now_us+=(int64_t)t*10000;if(!queue_full)return 0;memcpy(data,&queued_offer,sizeof(queued_offer));queue_full=false;return pdTRUE;}
int xTaskCreate(void(*fn)(void*),const char*n,unsigned stack,void*arg,unsigned pri,TaskHandle_t*task){(void)fn;(void)n;(void)stack;(void)arg;(void)pri;if(task_fail)return 0;*task=(void*)3;return pdPASS;}
TaskHandle_t xTaskGetCurrentTaskHandle(void){return in_worker?(void*)3:NULL;}
void vTaskDelay(TickType_t t){now_us+=(int64_t)t*10000;}
int64_t esp_timer_get_time(void){return now_us;}
uint32_t esp_random(void){return 0;}
void esp_restart(void){longjmp(stop,1);}
esp_err_t esp_flash_get_size(void *c,uint32_t*s){(void)c;*s=4194304;return ESP_OK;}
const esp_app_desc_t *esp_app_get_description(void){return &current_desc;}
const esp_partition_t *esp_ota_get_running_partition(void){return &parts[running];}
const esp_partition_t *esp_ota_get_boot_partition(void){return &parts[boot_selected];}
const esp_partition_t *esp_ota_get_next_update_partition(const esp_partition_t *p){(void)p;return &parts[1-running];}
const esp_partition_t *esp_partition_find_first(int type,int subtype,const char*l){(void)l;for(unsigned i=0;i<3;++i)if(parts[i].type==type&&parts[i].subtype==subtype)return &parts[i];return NULL;}
esp_err_t esp_partition_read(const esp_partition_t*p,size_t o,void*out,size_t n){assert(p==&parts[1]&&o+n<=flash_used);memcpy(out,flash_bytes+o,n);return ESP_OK;}
esp_err_t esp_ota_get_state_partition(const esp_partition_t*p,esp_ota_img_states_t*s){assert(p==&parts[0]||p==&parts[1]);if(missing_selector)return ESP_ERR_NOT_FOUND;*s=states[p==&parts[1]];return ESP_OK;}
esp_err_t esp_ota_get_partition_description(const esp_partition_t*p,esp_app_desc_t*d){assert(p==&parts[1]);memcpy(d,flash_bytes+32,sizeof(*d));return ESP_OK;}
esp_err_t esp_ota_mark_app_valid_cancel_rollback(void){++markvalid_count;states[running]=ESP_OTA_IMG_VALID;return ESP_OK;}
esp_err_t esp_ota_mark_app_invalid_rollback_and_reboot(void){++rollback_count;states[running]=ESP_OTA_IMG_INVALID;longjmp(stop,1);}
esp_err_t esp_ota_begin(const esp_partition_t*p,size_t size,esp_ota_handle_t*h){assert(p!=&parts[running]&&size==OTA_WITH_SEQUENTIAL_WRITES);++begin_count;*h=1;flash_used=0;return ESP_OK;}
esp_err_t esp_ota_write(esp_ota_handle_t h,const void*d,size_t n){assert(h==1 && flash_used+n<=sizeof(flash_bytes));if(write_error&&flash_used)return ESP_FAIL;memcpy(flash_bytes+flash_used,d,n);flash_used+=n;if(power_cut_after_write)longjmp(stop,1);return ESP_OK;}
esp_err_t esp_ota_end(esp_ota_handle_t h){assert(h==1);++end_count;return invalid_image_end?ESP_ERR_OTA_VALIDATE_FAILED:ESP_OK;}
esp_err_t esp_ota_abort(esp_ota_handle_t h){assert(h==1);++abort_count;return ESP_OK;}
esp_err_t esp_ota_set_boot_partition(const esp_partition_t*p){++setboot_count;if(p==&parts[running]&&missing_selector){if(selector_fail)return ESP_FAIL;missing_selector=false;states[running]=ESP_OTA_IMG_NEW;boot_selected=running;return ESP_OK;}rw_ota_journal_t j;assert(durable_size==sizeof(j));memcpy(&j,durable,sizeof(j));assert(j.pending_address==p->address && j.floor==6);if(selector_fail)return ESP_FAIL;boot_selected=p==&parts[1];states[boot_selected]=ESP_OTA_IMG_NEW;return ESP_OK;}
esp_err_t nvs_open(const char*n,int mode,nvs_handle_t*h){assert(!strcmp(n,"rw_ota")&&mode==NVS_READWRITE);*h=1;return ESP_OK;}
void nvs_close(nvs_handle_t h){assert(h==1);}
esp_err_t nvs_get_blob(nvs_handle_t h,const char*k,void*out,size_t*n){assert(h==1&&strlen(k)==15);if(!durable_size)return ESP_ERR_NVS_NOT_FOUND;assert(*n>=durable_size);*n=durable_size;memcpy(out,durable,*n);return ESP_OK;}
esp_err_t nvs_set_blob(nvs_handle_t h,const char*k,const void*d,size_t n){assert(h==1&&strlen(k)==15&&n<=sizeof(staged));if(storage_fail)return ESP_FAIL;memcpy(staged,d,n);staged_size=n;return ESP_OK;}
esp_err_t nvs_commit(nvs_handle_t h){assert(h==1);if(storage_fail)return ESP_FAIL;memcpy(durable,staged,staged_size);durable_size=staged_size;return ESP_OK;}
esp_err_t rw_power_btn_quiesce(void){if(power_busy||power_reserved)return ESP_ERR_INVALID_STATE;power_reserved=true;return ESP_OK;}
void rw_power_btn_resume_actions(void){power_reserved=false;}
esp_err_t rw_hid_quiesce(void){if(hid_busy||hid_reserved)return ESP_ERR_INVALID_STATE;hid_reserved=true;return ESP_OK;}
void rw_hid_resume_actions(void){hid_reserved=false;}
esp_err_t esp_crt_bundle_attach(void*c){(void)c;return ESP_OK;}
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t*c){assert(c->crt_bundle_attach==esp_crt_bundle_attach&&c->disable_auto_redirect&&c->timeout_ms<=10000);struct http_mock*h=calloc(1,sizeof(*h));assert(h);strcpy(h->url,c->url);strcpy(last_url,c->url);return h;}
esp_err_t esp_http_client_set_header(esp_http_client_handle_t c,const char*k,const char*v){(void)c;assert(!strcmp(k,"Accept-Encoding")&&!strcmp(v,"identity"));return ESP_OK;}
esp_err_t esp_http_client_open(esp_http_client_handle_t c,int l){assert(!l);if(tls_fail)return ESP_FAIL;++opened;c->offset=0;if(strstr(c->url,".json")){c->body=(uint8_t*)manifest_json;c->size=strlen(manifest_json);}else{c->body=image;c->size=image_size;}return ESP_OK;}
int64_t esp_http_client_fetch_headers(esp_http_client_handle_t c){return header_size==-2?(int64_t)c->size:header_size;}
int esp_http_client_get_status_code(esp_http_client_handle_t c){return redirect_on&&!strstr(c->url,"token")?302:200;}
esp_err_t esp_http_client_set_redirection(esp_http_client_handle_t c){strcpy(c->url,redirect_bad?"http://evil.test/token":"https://release-assets.githubusercontent.com/token?signature=public");return ESP_OK;}
esp_err_t esp_http_client_get_url(esp_http_client_handle_t c,char*out,int n){if(strlen(c->url)>=(size_t)n)return ESP_ERR_INVALID_SIZE;strcpy(out,c->url);return ESP_OK;}
esp_err_t esp_http_client_close(esp_http_client_handle_t c){(void)c;return ESP_OK;}
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t c){if(c){++cleaned;free(c);}return ESP_OK;}
int esp_http_client_read(esp_http_client_handle_t c,char*out,int wanted){now_us+=read_delay;if(read_error&&c->offset)return -1;size_t n=c->size-c->offset;if(n>(size_t)wanted)n=wanted;if(n>read_limit)n=read_limit;memcpy(out,c->body+c->offset,n);c->offset+=n;return n;}
bool esp_http_client_is_complete_data_received(esp_http_client_handle_t c){return c->offset==c->size;}
