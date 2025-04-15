#include "file_server.h"
#include "esp_log.h"

static const char *TAG = "download_handler";

esp_err_t download_get_handler(httpd_req_t *req)
{
    struct file_server_data *server_data = req->user_ctx;
    char filepath[FILE_PATH_MAX];
    const char *filename = get_path_from_uri(filepath, server_data->base_path, req->uri, sizeof(filepath));
    
    if (!filename) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Filename too long");
        return ESP_FAIL;
    }

    // Special handling for root path
    if (strcmp(filename, "/") == 0) {
        return send_index_html(req);
    }

    // Check for other static files
    if (strcmp(filename, "/index.html") == 0) return send_index_html(req);
    if (strcmp(filename, "/favicon.ico") == 0) return favicon_get_handler(req);
    if (strcmp(filename, "/style.css") == 0) return style_css_get_handler(req);
    if (strcmp(filename, "/script.js") == 0) return script_js_handler(req);

    struct stat file_stat;
    if (stat(filepath, &file_stat) == -1) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File does not exist");
        return ESP_FAIL;
    }

    FILE *fd = fopen(filepath, "r");
    if (!fd) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to read file");
        return ESP_FAIL;
    }

    set_content_type_from_file(req, filename);
    httpd_resp_set_hdr(req, "Connection", "close");

    char *chunk = server_data->scratch;
    size_t chunksize;
    while ((chunksize = fread(chunk, 1, SCRATCH_BUFSIZE, fd)) > 0) {
        if (httpd_resp_send_chunk(req, chunk, chunksize) != ESP_OK) {
            fclose(fd);
            httpd_resp_sendstr_chunk(req, NULL);
            return ESP_FAIL;
        }
    }

    fclose(fd);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}