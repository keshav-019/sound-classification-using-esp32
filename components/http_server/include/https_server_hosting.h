#include <esp_wifi.h>
#include <esp_event.h>
#include <esp_log.h>
#include <esp_system.h>
#include <nvs_flash.h>
#include <sys/param.h>
#include "esp_netif.h"
#include "esp_eth.h"
#include "protocol_examples_common.h"

#include <esp_https_server.h>
#include "esp_tls.h"
#include "sdkconfig.h"



static void event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data);

static esp_err_t root_get_handler(httpd_req_t *req);

static void print_peer_cert_info(const mbedtls_ssl_context *ssl);

static void https_server_user_callback(esp_https_server_user_cb_arg_t *user_cb);

static httpd_handle_t start_webserver();

static esp_err_t stop_webserver(httpd_handle_t server);

static void disconnect_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data);

static void connect_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data);


