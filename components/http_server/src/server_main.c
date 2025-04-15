#include "file_server.h"
#include "esp_log.h"
#include "esp_http_server.h"

static const char *TAG = "server_main";
static struct file_server_data *server_data = NULL;

static void httpd_close_func(httpd_handle_t hd, int sockfd)
{
    shutdown(sockfd, SHUT_RDWR);
    close(sockfd);
    ESP_LOGD(TAG, "Forcefully closed socket %d", sockfd);
}

esp_err_t start_file_server(const char *base_path)
{
    if (server_data) return ESP_ERR_INVALID_STATE;

    server_data = calloc(1, sizeof(struct file_server_data));
    if (!server_data) return ESP_ERR_NO_MEM;
    strlcpy(server_data->base_path, base_path, sizeof(server_data->base_path));

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.keep_alive_enable = false;
    config.lru_purge_enable = true;
    config.max_uri_handlers = 20;
    config.close_fn = httpd_close_func;

    httpd_handle_t server = NULL;
    if (httpd_start(&server, &config) != ESP_OK) {
        free(server_data);
        return ESP_FAIL;
    }

    const httpd_uri_t handlers[] = {
        {.uri = "/", .method = HTTP_GET, .handler = send_index_html, .user_ctx = server_data},
        {.uri = "/index.html", .method = HTTP_GET, .handler = send_index_html, .user_ctx = server_data},
        {.uri = "/style.css", .method = HTTP_GET, .handler = style_css_get_handler, .user_ctx = server_data},
        {.uri = "/script.js", .method = HTTP_GET, .handler = script_js_handler, .user_ctx = server_data},
        {.uri = "/favicon.ico", .method = HTTP_GET, .handler = favicon_get_handler, .user_ctx = server_data},
        {.uri = "/upload/*", .method = HTTP_POST, .handler = upload_post_handler, .user_ctx = server_data},
        {.uri = "/delete/*", .method = HTTP_POST, .handler = delete_post_handler, .user_ctx = server_data},
        {.uri = "/start_recording*", .method = HTTP_GET, .handler = start_recording_handler, .user_ctx = server_data},
        {.uri = "/list_files", .method = HTTP_GET, .handler = list_files_handler, .user_ctx = NULL},
        {.uri = "/delete_file", .method = HTTP_GET, .handler = delete_file_handler, .user_ctx = NULL},
        {.uri = "/download_file", .method = HTTP_GET, .handler = download_file_handler, .user_ctx = NULL},
        {.uri = "/*", .method = HTTP_GET, .handler = download_get_handler, .user_ctx = server_data},
    };

    for (size_t i = 0; i < sizeof(handlers)/sizeof(handlers[0]); i++) {
        httpd_register_uri_handler(server, &handlers[i]);
    }

    return ESP_OK;
}