#include <stdio.h>
#include <string.h>
#include <sys/param.h>
#include <sys/unistd.h>
#include <sys/stat.h>
#include <dirent.h>

#include "esp_err.h"
#include "esp_log.h"

#include "esp_vfs.h"
#include "esp_spiffs.h"
#include "esp_http_server.h"
#include "https_server_hosting.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_https_server.h"
#include "esp_tls.h"


/* Event handler for server events */
static void event_handler(void* arg, esp_event_base_t event_base,
    int32_t event_id, void* event_data)
{
    if (event_base == ESP_HTTPS_SERVER_EVENT) {
        if (event_id == HTTPS_SERVER_EVENT_ERROR) {
            esp_https_server_last_error_t *last_error = (esp_https_server_last_error_t *) event_data;
            ESP_LOGE(TAG, "Error: %s (TLS error: 0x%x)", 
            esp_err_to_name(last_error->last_error),
            last_error->esp_tls_error_code);
        }
    }
}

struct file_server_data {
    /* Base path of file storage */
    char base_path[ESP_VFS_PATH_MAX + 1];

    /* Scratch buffer for temporary storage during file transfer */
    char scratch[SCRATCH_BUFSIZE];
};

static const char *TAG = "file_server";

/* Handler to redirect incoming GET request for /index.html to /
 * This can be overridden by uploading file with same name */
static esp_err_t index_html_get_handler(httpd_req_t *req)
{
    httpd_resp_set_status(req, "307 Temporary Redirect");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_send(req, NULL, 0);  // Response body can be empty
    return ESP_OK;
}

static esp_err_t script_js_handler(httpd_req_t *req)
{
    extern const unsigned char script_js_start[]    asm("_binary_script_js_start");
    extern const unsigned char script_js_end[]      asm("_binary_script_js_end");
    const size_t script_js_size = (script_js_end - script_js_start);
    httpd_resp_set_type(req, "application/javascript");
    httpd_resp_send(req, (const char *)script_js_start, script_js_size);
    return ESP_OK;
}

static esp_err_t server_certificate_handler(httpd_req_t *req) {
    extern const unsigned char servercert_start[] asm("_binary_servercert_pem_start");
    extern const unsigned char servercert_end[] asm("_binary_servercert_pem_end");
    const size_t servercert_size = servercert_end - servercert_start;
    httpd_resp_set_type(req, "application/x-pem-file");
    httpd_resp_send(req, (const char *)servercert_start, servercert_size);
    return ESP_OK;
}

static esp_err_t style_css_get_handler(httpd_req_t *req){
    extern const unsigned char style_css_start[]    asm("_binary_style_css_start");
    extern const unsigned char style_css_end[]      asm("_binary_style_css_end");
    const size_t style_css_size = (style_css_end - style_css_start);
    httpd_resp_set_type(req, "text/css");
    httpd_resp_send(req, (const char *)style_css_start, style_css_size);
    return ESP_OK;
}

static esp_err_t send_file(httpd_req_t *req, const char *filepath) {
    char *chunk = ((struct file_server_data *)req->user_ctx)->scratch;
    FILE *fd = fopen(filepath, "r");
    if (!fd) {
        ESP_LOGE(TAG, "Failed to read file : %s", filepath);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to read file");
        return ESP_FAIL;
    }

    // Set MIME type
    if (strstr(filepath, ".html")) {
        httpd_resp_set_type(req, "text/html");
    } else if (strstr(filepath, ".css")) {
        httpd_resp_set_type(req, "text/css");
    } else if (strstr(filepath, ".js")) {
        httpd_resp_set_type(req, "application/javascript");
    } else if (strstr(filepath, ".ico")) {
        httpd_resp_set_type(req, "image/x-icon");
    } else {
        httpd_resp_set_type(req, "text/plain");
    }

    size_t chunksize;
    do {
        chunksize = fread(chunk, 1, SCRATCH_BUFSIZE, fd);
        if (chunksize > 0) {
            if (httpd_resp_send_chunk(req, chunk, chunksize) != ESP_OK) {
                fclose(fd);
                ESP_LOGE(TAG, "File sending failed!");
                httpd_resp_sendstr_chunk(req, NULL);
                return ESP_FAIL;
            }
        }
    } while (chunksize != 0);
    fclose(fd);
    httpd_resp_send_chunk(req, NULL, 0); // End response
    return ESP_OK;
}

static esp_err_t file_get_handler(httpd_req_t *req) {
    char filepath[FILE_PATH_MAX]; // Use maximum possible size
    struct file_server_data *server = (struct file_server_data *)req->user_ctx;

    const char *uri = req->uri;
    if (strcmp(uri, "/") == 0) {
        uri = "/index.html";
    }

    snprintf(filepath, sizeof(filepath), "%s%s", server->base_path, uri);
    return send_file(req, filepath);
}

static httpd_handle_t start_https_server(const char *base_path) {
    struct file_server_data *server_data = calloc(1, sizeof(struct file_server_data));
    strlcpy(server_data->base_path, "/spiffs", sizeof(server_data->base_path));

    httpd_ssl_config_t conf = HTTPD_SSL_CONFIG_DEFAULT();
    
    // Certificate configuration
    extern const unsigned char servercert_start[] asm("_binary_servercert_pem_start");
    extern const unsigned char servercert_end[]   asm("_binary_servercert_pem_end");
    conf.servercert = servercert_start;
    conf.servercert_len = servercert_end - servercert_start;

    extern const unsigned char prvtkey_pem_start[] asm("_binary_prvtkey_pem_start");
    extern const unsigned char prvtkey_pem_end[]   asm("_binary_prvtkey_pem_end");
    conf.prvtkey_pem = prvtkey_pem_start;
    conf.prvtkey_len = prvtkey_pem_end - prvtkey_pem_start;

    // Start server
    esp_err_t ret = httpd_ssl_start(&server, &conf);
    if (ESP_OK != ret) {
        ESP_LOGE(TAG, "Server start failed!");
        free(server_data);
        return NULL;
    }

    httpd_uri_t index_html_uri = {
        .uri       = "/index.html",
        .method    = HTTP_GET,
        .handler   = index_html_get_handler,
        .user_ctx  = server_data
    };
    httpd_register_uri_handler(server, &index_html_uri);

    httpd_uri_t style_css_uri = {
        .uri        = "/style.css",
        .method     = HTTP_GET,
        .handler    = style_css_get_handler,
        .user_ctx   = server_data
    };
    httpd_register_uri_handler(server, &style_css_uri);

    httpd_uri_t script_js_uri = {
        .uri        = "/script.js",
        .method     = HTTP_GET,
        .handler    = script_js_handler,
        .user_ctx   = server_data
    };
    httpd_register_uri_handler(server, &script_js_uri);

    httpd_uri_t file_get_uri = {
        .uri       = "/*",
        .method    = HTTP_GET,
        .handler   = file_get_handler,
        .user_ctx  = server_data
    };
    httpd_register_uri_handler(server, &file_get_uri);

    // Register certificate handler
    httpd_uri_t cert_uri = {
        .uri = "/server.crt",
        .method = HTTP_GET,
        .handler = server_certificate_handler,
        .user_ctx = server_data
    };
    httpd_register_uri_handler(server, &cert_uri);

    return server;
}


void stop_file_server(void)
{
    if (server) {
        httpd_ssl_stop(server);
        server = NULL;
    }
}

