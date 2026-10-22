#pragma once
/* Request/response fake: a request carries its body, a response is captured
 * as status + text so tests read exactly what a client would receive. */
#include <stddef.h>
#include <sys/types.h>
#include "esp_err.h"

typedef void *httpd_handle_t;
enum { HTTP_GET = 1, HTTP_POST = 3 };
#define HTTPD_SOCK_ERR_TIMEOUT (-3)
typedef enum { HTTPD_404_NOT_FOUND = 404 } httpd_err_code_t;

typedef struct httpd_req {
    int method;
    size_t content_len;
    const char *body;
    size_t body_off;
    char status[48];
    char resp[4096];
    int sent;
} httpd_req_t;

typedef struct {
    const char *uri;
    int method;
    esp_err_t (*handler)(httpd_req_t *req);
    void *user_ctx;
} httpd_uri_t;

typedef struct {
    int server_port, max_uri_handlers, lru_purge_enable, stack_size;
} httpd_config_t;
#define HTTPD_DEFAULT_CONFIG() ((httpd_config_t){0})

esp_err_t httpd_start(httpd_handle_t *handle, const httpd_config_t *config);
esp_err_t httpd_stop(httpd_handle_t handle);
esp_err_t httpd_register_uri_handler(httpd_handle_t handle, const httpd_uri_t *uri);
esp_err_t httpd_register_err_handler(httpd_handle_t handle, httpd_err_code_t error,
                                     esp_err_t (*handler)(httpd_req_t *, httpd_err_code_t));
int httpd_req_recv(httpd_req_t *req, char *buf, size_t len);
size_t httpd_req_get_hdr_value_len(httpd_req_t *req, const char *field);
esp_err_t httpd_req_get_hdr_value_str(httpd_req_t *req, const char *field, char *val, size_t len);
esp_err_t httpd_resp_set_status(httpd_req_t *req, const char *status);
esp_err_t httpd_resp_set_type(httpd_req_t *req, const char *type);
esp_err_t httpd_resp_set_hdr(httpd_req_t *req, const char *field, const char *value);
esp_err_t httpd_resp_sendstr(httpd_req_t *req, const char *str);
esp_err_t httpd_resp_send(httpd_req_t *req, const char *buf, ssize_t len);
esp_err_t httpd_resp_send_err(httpd_req_t *req, httpd_err_code_t error, const char *msg);
