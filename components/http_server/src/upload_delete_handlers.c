#include "file_server.h"
#include "esp_log.h"

static const char *TAG = "upload_delete";

esp_err_t upload_post_handler(httpd_req_t *req)
{
    struct file_server_data *server_data = req->user_ctx;
    char filepath[FILE_PATH_MAX];
    
    const char *filename = get_path_from_uri(filepath, server_data->base_path,
                                           req->uri + sizeof("/upload") - 1, sizeof(filepath));
    if (!filename) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Filename too long");
        return ESP_FAIL;
    }

    /* File upload implementation here */
    /* ... */

    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_set_hdr(req, "Connection", "close");
    return httpd_resp_sendstr(req, "File uploaded successfully");
}

esp_err_t delete_post_handler(httpd_req_t *req)
{
    struct file_server_data *server_data = req->user_ctx;
    char filepath[FILE_PATH_MAX];
    
    const char *filename = get_path_from_uri(filepath, server_data->base_path,
                                           req->uri + sizeof("/delete") - 1, sizeof(filepath));
    if (!filename) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Filename too long");
        return ESP_FAIL;
    }

    /* File deletion implementation here */
    /* ... */

    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_set_hdr(req, "Connection", "close");
    return httpd_resp_sendstr(req, "File deleted successfully");
}