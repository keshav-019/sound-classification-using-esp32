#include <stdio.h>
#include <string.h>
#include <sys/param.h>
#include <sys/unistd.h>
#include <sys/stat.h>
#include <dirent.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_vfs.h"
#include "esp_spiffs.h"
#include "esp_http_server.h"
#include "i2s_recorder_main.h"
#include "file_server.h"
#include "esp_vfs_fat.h"
#include "file_operations.h"
#include "esp_timer.h"

static const char *TAG = "file_server";

/**
 * @brief Extracts filesystem path from URI
 * @param dest Destination buffer for path
 * @param base_path Base filesystem path (e.g., "/sdcard")
 * @param uri Request URI (e.g., "/files/data.txt")
 * @param destsize Size of destination buffer
 * @return Pointer to path relative to base_path, or NULL if path too long
 * 
 * @example 
 * char path[256];
 * get_path_from_uri(path, "/sdcard", "/recordings/audio.wav", sizeof(path));
 * // Returns pointer to "/recordings/audio.wav" within path buffer
 */
static const char* get_path_from_uri(char *dest, const char *base_path, const char *uri, size_t destsize)
{
    const size_t base_pathlen = strlen(base_path);
    size_t pathlen = strlen(uri);

    const char *quest = strchr(uri, '?');
    if (quest) pathlen = MIN(pathlen, quest - uri);
    const char *hash = strchr(uri, '#');
    if (hash) pathlen = MIN(pathlen, hash - uri);

    if (base_pathlen + pathlen + 1 > destsize) return NULL;

    strcpy(dest, base_path);
    strlcpy(dest + base_pathlen, uri, pathlen + 1);
    return dest + base_pathlen;
}

/**
* @brief Sets HTTP Content-Type header based on file extension
* @param req HTTP request object
* @param filename Name of the file being served
* @return ESP_OK on success, error code otherwise
* 
* @note Supports common web file types: HTML, CSS, JS, images, PDF
*/
static esp_err_t set_content_type_from_file(httpd_req_t *req, const char *filename)
{
    if (strcasecmp(&filename[strlen(filename) - 4], ".pdf") == 0) {
        return httpd_resp_set_type(req, "application/pdf");
    } else if (strcasecmp(&filename[strlen(filename) - 5], ".html") == 0) {
        return httpd_resp_set_type(req, "text/html");
    } else if (strcasecmp(&filename[strlen(filename) - 4], ".jpg") == 0 ||
            strcasecmp(&filename[strlen(filename) - 5], ".jpeg") == 0) {
        return httpd_resp_set_type(req, "image/jpeg");
    } else if (strcasecmp(&filename[strlen(filename) - 4], ".png") == 0) {
        return httpd_resp_set_type(req, "image/png");
    } else if (strcasecmp(&filename[strlen(filename) - 4], ".ico") == 0) {
        return httpd_resp_set_type(req, "image/x-icon");
    } else if (strcasecmp(&filename[strlen(filename) - 3], ".js") == 0) {
        return httpd_resp_set_type(req, "application/javascript");
    } else if (strcasecmp(&filename[strlen(filename) - 4], ".css") == 0) {
        return httpd_resp_set_type(req, "text/css");
    }
    return httpd_resp_set_type(req, "text/plain");
}

/**
* @brief Forcefully closes a HTTP connection
* @param hd HTTP server handle
* @param sockfd Socket file descriptor to close
* 
* @note Used as a callback for clean connection termination
*/
static void httpd_close_func(httpd_handle_t hd, int sockfd)
{
    shutdown(sockfd, SHUT_RDWR);
    close(sockfd);
    ESP_LOGD(TAG, "Forcefully closed socket %d", sockfd);
}

/**
* @brief Timer callback to stop recording after timeout
* @param arg Pointer to recording flag (bool)
*/
static void recording_timer_callback(void* arg) {
    bool* recording_flag = (bool*)arg;
    *recording_flag = false;
    ESP_LOGI(TAG, "Recording timer expired - flag set to false");
}

/**
* @brief Starts audio recording via HTTP request
* @param req HTTP request containing 'category' query parameter
* @return ESP_OK on success, error code on failure
* 
* @example 
* GET /start_recording?category=voice
* 
* @note Creates a new task for recording and sets a timer to stop it
*/
esp_err_t start_recording_handler(httpd_req_t *req) {
    static bool is_recording = false;
    
    if (is_recording) {
        httpd_resp_send_custom_err(req, HTTPD_400, "Recording already in progress");
        return ESP_FAIL;
    }

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

    is_recording = true;
    xTaskCreate(
        (TaskFunction_t)start_recording,
        "rec_task",
        4096,
        (void*)task_category,
        5,
        NULL
    );

    // Timer setup
    const esp_timer_create_args_t timer_args = {
        .callback = &recording_timer_callback,
        .arg = &is_recording,
        .name = "recording_timer"
    };
    
    esp_timer_handle_t timer;
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &timer));
    ESP_ERROR_CHECK(esp_timer_start_once(timer, CONFIG_EXAMPLE_REC_TIME * 1000000 + 1000000));

    httpd_resp_send(req, "Recording started", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/**
* @brief Sanitizes file paths to prevent directory traversal
* @param input Raw input path
* @param output Buffer for sanitized output
* @param output_len Size of output buffer
* @return ESP_OK on success, ESP_FAIL on invalid path
* 
* @note Blocks paths containing "../" or "..\"
*/
esp_err_t sanitize_path(const char *input, char *output, size_t output_len) {
    if (!input || !output || output_len == 0) {
        return ESP_FAIL;
    }

    size_t i = 0;
    while (*input && i < output_len - 1) {
        if (isalnum((unsigned char)*input) || *input == '_' || *input == '-' || 
            *input == '/' || (*input == '.' && !(input[0] == '.' && input[1] == '/'))) {
            output[i++] = *input;
        }
        input++;
    }
    output[i] = '\0';
    
    if (strstr(output, "../") || strstr(output, "..\\")) {
        return ESP_FAIL;
    }
    
    return ESP_OK;
}

/**
 * @brief HTTP handler for listing directory contents in JSON format
 * @param req HTTP request object
 * @return ESP_OK on success, appropriate error code on failure
 * 
 * @handles GET /list_files[?path=<directory>]
 * 
 * @response JSON structure:
 * {
 *   "path": "/sdcard",
 *   "files": [
 *     {
 *       "name": "file1.txt",
 *       "path": "/sdcard/file1.txt",
 *       "size": 1024,
 *       "type": "file",
 *       "modified": "2023-01-01 12:00:00"
 *     },
 *     {
 *       "name": "folder1",
 *       "path": "/sdcard/folder1",
 *       "size": 4096,
 *       "type": "directory",
 *       "modified": "2023-01-01 12:05:00"
 *     }
 *   ]
 * }
 * 
 * @note Requires mounted SD card. Path parameter is optional (defaults to root).
 */
esp_err_t list_files_handler(httpd_req_t *req) {
    if (req == NULL) {
        ESP_LOGE(TAG, "Null request pointer");
        return ESP_FAIL;
    }

    // First ensure SD card is mounted
    if (!sd_card_mounted) {
        mount_sdcard();
        if (!sd_card_mounted) {  // Verify it worked
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "SD card mount failed");
            return ESP_FAIL;
        }
    }

    // Build base path with safety checks
    char path[256];
    if (snprintf(path, sizeof(path), "%s", SD_MOUNT_POINT) >= sizeof(path)) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Path too long");
        return ESP_FAIL;
    }

    // Process query parameters if they exist
    size_t query_len = httpd_req_get_url_query_len(req) + 1; // +1 for null terminator
    if (query_len > 1) { // >1 because empty string would be just null terminator
        char* query = malloc(query_len);
        if (!query) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Memory allocation failed");
            return ESP_FAIL;
        }

        if (httpd_req_get_url_query_str(req, query, query_len) != ESP_OK) {
            free(query);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid query string");
            return ESP_FAIL;
        }

        char param[128];
        if (httpd_query_key_value(query, "path", param, sizeof(param)) == ESP_OK) {
            // Sanitize path input
            char sanitized[128];
            if (sanitize_path(param, sanitized, sizeof(sanitized)) != ESP_OK) {
                free(query);
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid path characters");
                return ESP_FAIL;
            }

            // Construct full path
            const char *format = (sanitized[0] == '/') ? "%s%s" : "%s/%s";
            if (snprintf(path, sizeof(path), format, SD_MOUNT_POINT, sanitized) >= sizeof(path)) {
                free(query);
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Path too long");
                return ESP_FAIL;
            }
        }
        free(query);
    }

    // Verify path exists and is a directory
    struct stat st;
    if (stat(path, &st) != 0) {
        ESP_LOGE(TAG, "stat failed for %s (errno %d)", path, errno);
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Path not found");
        return ESP_FAIL;
    }

    if (!S_ISDIR(st.st_mode)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Not a directory");
        return ESP_FAIL;
    }

    // Generate JSON listing
    char *json_response = list_files_json(path);
    if (!json_response) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to generate listing");
        return ESP_FAIL;
    }

    // Set response type before sending
    esp_err_t ret = httpd_resp_set_type(req, "application/json");
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set response type: %s", esp_err_to_name(ret));
        free(json_response);
        return ret;
    }

    // Send response
    ret = httpd_resp_send(req, json_response, strlen(json_response));
    free(json_response);
    
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send response: %s", esp_err_to_name(ret));
    }
    return ret;
}

/**
 * @brief HTTP handler for deleting files
 * @param req HTTP request containing filename in query string
 * @return ESP_OK on success, error code on failure
 * 
 * @handles GET /delete_file?filename=<path>
 * 
 * @example 
 * DELETE /delete_file?filename=recordings/old.wav
 * 
 * @response "File deleted successfully" or error message
 * 
 * @security Checks for valid path format before deletion
 */
esp_err_t delete_file_handler(httpd_req_t *req) {
    char filepath[MAX_PATH_LENGTH];
    char decoded_path[MAX_PATH_LENGTH];
    
    if (httpd_req_get_url_query_str(req, filepath, sizeof(filepath)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Filename required");
        return ESP_FAIL;
    }
    
    char full_path[MAX_PATH_LENGTH];
    if (snprintf(full_path, sizeof(full_path), "%s/%s", SD_MOUNT_POINT, decoded_path) >= sizeof(full_path)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Path too long");
        return ESP_FAIL;
    }
    
    if (unlink(full_path) != 0) {
        ESP_LOGE(TAG, "Failed to delete file: %s", full_path);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to delete file");
        return ESP_FAIL;
    }
    
    httpd_resp_sendstr(req, "File deleted successfully");
    return ESP_OK;
}

/**
 * @brief HTTP handler for downloading files
 * @param req HTTP request containing filename in query string
 * @return ESP_OK on success, error code on failure
 * 
 * @handles GET /download_file?filename=<path>
 * 
 * @response 
 * - Binary file content with Content-Disposition: attachment header
 * - Appropriate Content-Type based on file extension
 * 
 * @note Uses chunked transfer encoding for large files
 */
esp_err_t download_file_handler(httpd_req_t *req) {
    char filepath[MAX_PATH_LENGTH];
    char decoded_path[MAX_PATH_LENGTH];
    
    if (httpd_req_get_url_query_str(req, filepath, sizeof(filepath)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Filename required");
        return ESP_FAIL;
    }
    
    char full_path[MAX_PATH_LENGTH];
    if (snprintf(full_path, sizeof(full_path), "%s/%s", SD_MOUNT_POINT, decoded_path) >= sizeof(full_path)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Path too long");
        return ESP_FAIL;
    }
    
    FILE *file = fopen(full_path, "rb");
    if (!file) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File not found");
        return ESP_FAIL;
    }
    
    struct stat file_stat;
    if (stat(full_path, &file_stat) == -1) {
        fclose(file);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to get file stats");
        return ESP_FAIL;
    }
    
    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment");
    
    char *chunk = malloc(1024);
    if (!chunk) {
        fclose(file);
        return ESP_ERR_NO_MEM;
    }
    
    size_t read_bytes;
    while ((read_bytes = fread(chunk, 1, 1024, file)) > 0) {
        if (httpd_resp_send_chunk(req, chunk, read_bytes) != ESP_OK) {
            break;
        }
    }
    
    free(chunk);
    fclose(file);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

/**
 * @brief Serves embedded JavaScript file
 * @param req HTTP request object
 * @return ESP_OK on success
 * 
 * @handles GET /script.js
 * 
 * @response 
 * - Content-Type: application/javascript
 * - Embedded JavaScript file content
 */
esp_err_t script_js_handler(httpd_req_t *req)
{
    extern const unsigned char script_js_start[] asm("_binary_script_js_start");
    extern const unsigned char script_js_end[] asm("_binary_script_js_end");
    httpd_resp_set_type(req, "application/javascript");
    httpd_resp_set_hdr(req, "Connection", "close");
    return httpd_resp_send(req, (const char *)script_js_start, script_js_end - script_js_start);
}

/**
 * @brief Serves embedded CSS stylesheet
 * @param req HTTP request object
 * @return ESP_OK on success
 * 
 * @handles GET /style.css
 * 
 * @response 
 * - Content-Type: text/css
 * - Embedded CSS file content
 */
esp_err_t style_css_get_handler(httpd_req_t *req)
{
    extern const unsigned char style_css_start[] asm("_binary_style_css_start");
    extern const unsigned char style_css_end[] asm("_binary_style_css_end");
    httpd_resp_set_type(req, "text/css");
    httpd_resp_set_hdr(req, "Connection", "close");
    return httpd_resp_send(req, (const char *)style_css_start, style_css_end - style_css_start);
}

/**
 * @brief Serves embedded favicon
 * @param req HTTP request object
 * @return ESP_OK on success
 * 
 * @handles GET /favicon.ico
 * 
 * @response 
 * - Content-Type: image/x-icon
 * - Embedded favicon file content
 */
esp_err_t favicon_get_handler(httpd_req_t *req)
{
    extern const unsigned char favicon_ico_start[] asm("_binary_favicon_ico_start");
    extern const unsigned char favicon_ico_end[] asm("_binary_favicon_ico_end");
    httpd_resp_set_type(req, "image/x-icon");
    httpd_resp_set_hdr(req, "Connection", "close");
    return httpd_resp_send(req, (const char *)favicon_ico_start, favicon_ico_end - favicon_ico_start);
}

/**
 * @brief Serves the main HTML interface
 * @param req HTTP request object
 * @return ESP_OK on success, error code on failure
 * 
 * @handles GET / or /index.html
 * 
 * @response 
 * - Content-Type: text/html
 * - Embedded HTML file content
 * - Forces connection close after sending
 */
esp_err_t send_index_html(httpd_req_t *req)
{
    extern const unsigned char index_start[] asm("_binary_index_html_start");
    extern const unsigned char index_end[] asm("_binary_index_html_end");
    
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Connection", "close");
    esp_err_t ret = httpd_resp_send(req, (const char *)index_start, index_end - index_start);
    
    if (ret == ESP_OK) {
        httpd_sess_trigger_close(req->handle, httpd_req_to_sockfd(req));
    }
    return ret;
}

/**
 * @brief Handles file deletion via POST request
 * @param req HTTP request object containing path in URI
 * @return ESP_OK on success, error code on failure
 * 
 * @handles POST /delete/<path>
 * 
 * @response 
 * - 303 See Other redirect to home page
 * - Sets Connection: close header
 * 
 * @note Uses server_data context for base path resolution
 */
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
    delete_path(filename);
    /* ... */

    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_set_hdr(req, "Connection", "close");
    return httpd_resp_sendstr(req, "File deleted successfully");
}

/**
 * @brief Main file download handler with special cases
 * @param req HTTP request object
 * @return ESP_OK on success, error code on failure
 * 
 * @handles GET
 * 
 * @behavior
 * 1. Serves index.html for root path
 * 2. Handles known static files (CSS/JS/favicon)
 * 3. Sends regular files with proper Content-Type
 * 4. Uses chunked transfer for large files
 * 
 * @note Uses server_data context for base path and scratch buffer
 */
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

/**
 * @brief HTTP GET handler for real-time audio classification
 * @param req HTTP request object containing client connection details
 * @return ESP_OK on successful prediction and response, error code on failure
 * 
 * @handles GET /prediction
 * 
 * @response JSON response format:
 * {
 *   "category": "<predicted_class_name>"
 * }
 * OR error response:
 * {
 *   "error": "<error_description>"
 * }
 * 
 * @note This handler performs the following operations:
 * 1. Initializes I2S microphone if not already done
 * 2. Records 1024 audio samples (16-bit mono @16kHz)
 * 3. Converts samples to normalized float32 format
 * 4. Passes data to TensorFlow Lite model for inference
 * 5. Returns the top prediction class via HTTP and console
 * 
 * @section Class Mapping:
 * - 0: Alarm
 * - 1: Bell  
 * - 2: Crying Baby
 * - 3: Noise
 * - 4: Rain
 * - 5: Rooster
 * 
 * @section Error Handling:
 * - Returns HTTP 500 with JSON error on:
 *   - Audio recording failure
 *   - Invalid prediction result
 *   - HTTP response failure
 * - Always logs prediction results to console (visible even if HTTP fails)
 * 
 * @section Performance:
 * - Typical execution time: <100ms (including 64ms audio capture)
 * - Memory: Requires ~8KB for audio buffer + model tensors
 * - Blocks during audio capture and inference
 * 
 * @see init_microphone() for I2S configuration
 * @see predict_class() for model inference implementation
 */
static esp_err_t prediction_handler(httpd_req_t *req) {
    // Initialize microphone if not already done
    static bool mic_initialized = false;
    if (!mic_initialized) {
        init_microphone();
        mic_initialized = true;
    }

    i2s_chan_handle_t rx_handle = NULL;       ///< I2S microphone handle

    // Class name mapping
    const char* class_names[] = {
        "Alarm", 
        "Bell", 
        "Crying Baby", 
        "Noise", 
        "Rain", 
        "Rooster"
    };

    // Record 1024 samples (16-bit mono)
    int16_t audio_buffer[1024];
    size_t bytes_read;
    esp_err_t ret = i2s_channel_read(rx_handle, 
                                    (char *)audio_buffer, 
                                    sizeof(audio_buffer), 
                                    &bytes_read, 
                                    1000); // 1s timeout

    if (ret != ESP_OK || bytes_read != sizeof(audio_buffer)) {
        ESP_LOGE(TAG, "Failed to read audio samples: %s", esp_err_to_name(ret));
        const char* error_response = "{\"error\":\"Failed to record audio\"}";
        httpd_resp_send(req, error_response, strlen(error_response));
        return ESP_FAIL;
    }

    // Convert int16 samples to float32 for model input
    float float_buffer[1024];
    for (int i = 0; i < 1024; i++) {
        float_buffer[i] = (float)audio_buffer[i] / 32768.0f; // Normalize to [-1, 1]
    }

    // Get prediction
    int predicted_class = predict_class(float_buffer);
    if (predicted_class < 0 || predicted_class > 5) {
        ESP_LOGE(TAG, "Invalid prediction result: %d", predicted_class);
        const char* error_response = "{\"error\":\"Invalid prediction result\"}";
        httpd_resp_send(req, error_response, strlen(error_response));
        return ESP_FAIL;
    }

    // Log prediction result
    ESP_LOGI(TAG, "Predicted class: %d - %s", predicted_class, class_names[predicted_class]);

    // Prepare JSON response
    char response[128];
    snprintf(response, sizeof(response), 
             "{\"category\":\"%s\"}", 
             class_names[predicted_class]);

    // Send HTTP response
    httpd_resp_set_type(req, "application/json");
    ret = httpd_resp_send(req, response, strlen(response));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send HTTP response: %s", esp_err_to_name(ret));
    }

    return ret;
}

/**
 * @brief Initializes and starts the HTTP file server
 * @param base_path Root filesystem path to serve files from (e.g., "/sdcard")
 * @return ESP_OK on success, error code on failure
 * 
 * @note Registers multiple URI handlers for:
 * - File operations (list/delete/download)
 * - Static file serving
 * - Recording control
 * - Web interface
 */
esp_err_t start_file_server(const char *base_path)
{
    static struct file_server_data *server_data = NULL;
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
        {.uri = "/delete/*", .method = HTTP_POST, .handler = delete_post_handler, .user_ctx = server_data},
        {.uri = "/start_recording*", .method = HTTP_GET, .handler = start_recording_handler, .user_ctx = server_data},
        {.uri = "/list_files", .method = HTTP_GET, .handler = list_files_handler, .user_ctx = NULL},
        {.uri = "/delete_file", .method = HTTP_GET, .handler = delete_file_handler, .user_ctx = NULL},
        {.uri = "/download_file", .method = HTTP_GET, .handler = download_file_handler, .user_ctx = NULL},
        {.uri = "/predict", .method = HTTP_GET, .handler = prediction_handler, .user_ctx = server_data},
        {.uri = "/*", .method = HTTP_GET, .handler = download_get_handler, .user_ctx = server_data},
    };

    for (size_t i = 0; i < sizeof(handlers)/sizeof(handlers[0]); i++) {
        httpd_register_uri_handler(server, &handlers[i]);
    }

    ESP_LOGI(TAG, "File server started on path: %s", base_path);
    return ESP_OK;
}