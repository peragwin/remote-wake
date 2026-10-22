#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sodium.h>
#include "ota_manifest.h"
#include "ota_journal.h"
#include "b64url.h"
#include "cJSON.h"
static const unsigned char seed[32]={0x9d,0x61,0xb1,0x9d,0xef,0xfd,0x5a,0x60,0xba,0x84,0x4a,0xf4,0x92,0xec,0x2c,0xc4,0x44,0x49,0xc5,0x69,0x7b,0x32,0x69,0x19,0x70,0x3b,0xac,0x03,0x1c,0xae,0x7f,0x60};
static unsigned char secret[64];
static rw_ota_image_identity_t identity={.magic=RW_OTA_IDENTITY_MAGIC_BYTES,.format_version=1,.updater_api=1,.release=6,
    .product="remote-wake",.board="esp32-s3-devkitc",.profile="hid-power",.channel="stable",.layout="rw-4m-ab-v1",.version="1.2.2",.chip="esp32s3"};
static rw_ota_trusted_key_t key={.kid="test-rfc8032",.generation=1};
static rw_ota_config_t config={.image_identity=&identity,.trusted_keys=&key,.trusted_key_count=1,
    .source_prefix="https://github.com/example/remote-wake/releases/"};
static char original_payload[RW_OTA_PAYLOAD_MAX+1];
static char *seal(const char *payload)
{
    unsigned char message[RW_OTA_PAYLOAD_MAX+sizeof(RW_OTA_SIGNATURE_DOMAIN)],sig[64];
    size_t prefix=strlen(RW_OTA_SIGNATURE_DOMAIN),n=strlen(payload);
    assert(n<=RW_OTA_PAYLOAD_MAX);
    memcpy(message,RW_OTA_SIGNATURE_DOMAIN,prefix); memcpy(message+prefix,payload,n);
    crypto_sign_detached(sig,NULL,message,prefix+n,secret);
    char encoded[RW_B64URL_ENC_LEN(RW_OTA_PAYLOAD_MAX)+1],signature[87];
    assert(rw_b64url_encode((unsigned char*)payload,n,encoded,sizeof(encoded))>0);
    assert(rw_b64url_encode(sig,64,signature,sizeof(signature))==86);
    cJSON *root=cJSON_CreateObject();cJSON_AddNumberToObject(root,"v",1);cJSON_AddStringToObject(root,"kid",key.kid);
    cJSON_AddStringToObject(root,"payload",encoded);cJSON_AddStringToObject(root,"sig",signature);
    char *json=cJSON_PrintUnformatted(root);cJSON_Delete(root);return json;
}
static esp_err_t verify(const char *json,uint32_t floor)
{
    rw_ota_manifest_t out;
    return rw_ota_manifest_verify(&config,json,strlen(json),true,1750000000,4*1024*1024,0,floor,&out);
}
static void mutate(const char *name,const char *value,bool numeric)
{
    cJSON *p=cJSON_Parse(original_payload);assert(p);
    cJSON_ReplaceItemInObjectCaseSensitive(p,name,numeric?cJSON_CreateNumber(strtod(value,NULL)):cJSON_CreateString(value));
    char *raw=cJSON_PrintUnformatted(p),*json=seal(raw);assert(verify(json,6)!=ESP_OK);
    free(json);free(raw);cJSON_Delete(p);
}
static void journal_power_failures(void)
{
    uint8_t scope[32]={1},hash[32]={2};
    rw_ota_journal_t durable={.format=1,.floor=6};memcpy(durable.scope,scope,32);
    assert(rw_ota_journal_valid(&durable,scope));
    rw_ota_journal_t pending=durable;
    rw_ota_journal_prepare(&pending,7,0x1f0000,hash);
    /* Power loss before journal commit or boot selection leaves floor6 + old
     * running slot. A prepared journal never raises the confirmed floor. */
    assert(durable.floor==6 && !durable.pending_release && pending.floor==6);
    durable=pending;assert(durable.pending_release==7 && durable.floor==6);
    /* Failed boot/power loss during first boot: old image observes invalid or
     * aborted candidate, persists its hash, and refuses identical retries. */
    rw_ota_journal_reject(&durable,hash);
    assert(durable.floor==6 && !durable.pending_release && rw_ota_journal_rejected(&durable,hash));
    uint8_t newer[32]={3};assert(!rw_ota_journal_rejected(&durable,newer));
    rw_ota_journal_prepare(&durable,8,0x1f0000,newer);
    rw_ota_journal_confirm(&durable,8);
    assert(durable.floor==8 && !durable.pending_release);
    /* A rollback image below the confirmed floor may run, but manifests can
     * never move below the durable floor. */
    char *json=seal(original_payload);assert(verify(json,durable.floor)!=ESP_OK);free(json);
    /* Power failure after IDF mark-valid but before NVS floor commit: the new
     * running image's embedded release independently closes that gap. */
    uint32_t old=identity.release;identity.release=7;
    json=seal(original_payload);assert(verify(json,6)!=ESP_OK);free(json);identity.release=old;
    rw_ota_journal_confirm(&durable,6);assert(durable.floor==8);
    durable.failed_count=5;assert(!rw_ota_journal_valid(&durable,scope));
}
int main(int argc,char **argv)
{
    assert(argc==2 && sodium_init()>=0);crypto_sign_seed_keypair(key.public_key,secret,seed);
    FILE *f=fopen(argv[1],"rb");assert(f);assert(!fseek(f,0,SEEK_END));long len=ftell(f);rewind(f);
    char *fixture=calloc(1,len+1);assert(fixture && fread(fixture,1,len,f)==(size_t)len);fclose(f);
    cJSON *root=cJSON_Parse(fixture);assert(root);cJSON *e=cJSON_GetObjectItemCaseSensitive(root,"envelope");
    char *json=cJSON_PrintUnformatted(e);assert(verify(json,6)==ESP_OK);
    rw_ota_manifest_t manifest;
    assert(rw_ota_manifest_verify(&config,json,strlen(json),true,1750000000,4*1024*1024,0,6,&manifest)==ESP_OK);
    assert(manifest.identity.release==7 && !strcmp(manifest.identity.version,"1.2.3"));
    assert(rw_ota_manifest_verify(&config,json,strlen(json),false,1750000000,4*1024*1024,0,6,&manifest)!=ESP_OK);
    const char *payload=cJSON_GetObjectItemCaseSensitive(e,"payload")->valuestring;
    int decoded=rw_b64url_decode(payload,strlen(payload),(unsigned char*)original_payload,sizeof(original_payload)-1);assert(decoded>0);original_payload[decoded]=0;
    mutate("product","other",false);mutate("board","other",false);mutate("profile","other",false);
    mutate("channel","beta",false);mutate("layout","other",false);mutate("chip","esp32c3",false);
    mutate("release","6",true);mutate("release","4294967296",true);mutate("release","7.5",true);
    mutate("min_updater","2",true);mutate("min_flash","8388608",true);mutate("min_psram","1",true);
    mutate("published","1800000000",true);mutate("expires","1749999999",true);mutate("key_generation","2",true);
    mutate("url","http://github.com/example/remote-wake/releases/app.bin",false);
    mutate("url","https://github.com/example/other/releases/app.bin",false);mutate("sha256","BAD",false);
    char duplicate[RW_OTA_PAYLOAD_MAX+1];snprintf(duplicate,sizeof(duplicate),"{\"v\":1,%s",original_payload+1);
    char *bad=seal(duplicate);assert(verify(bad,6)!=ESP_OK);free(bad);
    snprintf(duplicate,sizeof(duplicate),"{\"unexpected\":{},%s",original_payload+1);bad=seal(duplicate);assert(verify(bad,6)!=ESP_OK);free(bad);
    char *sig=cJSON_GetObjectItemCaseSensitive(e,"sig")->valuestring;sig[0]=sig[0]=='A'?'B':'A';
    bad=cJSON_PrintUnformatted(e);assert(verify(bad,6)!=ESP_OK);free(bad);
    assert(!rw_ota_url_in_source("https://github.com@example.com/example/remote-wake/releases/a",config.source_prefix));
    assert(!rw_ota_url_in_source("https://github.com/example/remote-wake/releases/%2e%2e/a",config.source_prefix));
    assert(!rw_ota_url_in_source("https://github.com/example/remote-wake/releases/../a",config.source_prefix));
    assert(!rw_ota_url_in_source("https://github.com/example/remote-wake/releases/a?token=x",config.source_prefix));
    journal_power_failures();
    free(json);cJSON_Delete(root);free(fixture);
    puts("OTA real Ed25519 cross-language manifest, rejection and power-loss journal tests passed");
}
