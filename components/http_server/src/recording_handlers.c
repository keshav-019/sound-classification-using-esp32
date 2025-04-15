#include "file_server.h"
#include "i2s_recorder_main.h"
#include "esp_log.h"

static const char *TAG = "recording_handler";

esp_err_t start_recording_handler(httpd_req_t *req) {
    char query[64];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        httpd_resp_send_custom_err(req, HTTPD_400, "Query required");
        return ESP_FAIL;
    }

    char category[32];
    if (httpd_query_key_value(query, "category", category, sizeof(category)) != ESP_OK) {
        httpd_resp_send_custom_err(req, HTTPD_400, "Category missing");
        return ESP_FAIL;
    }

    char *task_category = strdup(category);
    if (!task_category) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    xTaskCreate(
        (TaskFunction_t)start_recording,
        "rec_task",
        4096,
        (void*)task_category,
        5,
        NULL
    );

    httpd_resp_send(req, "Recording started", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}