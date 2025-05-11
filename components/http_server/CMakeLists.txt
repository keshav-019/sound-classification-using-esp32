idf_component_register(SRCS "src/file_server.c"
                    INCLUDE_DIRS "include"
                    PRIV_REQUIRES vfs spiffs esp_http_server esp_http_client esp-tls recorder esp_driver_i2s fatfs file_operations esp_timer
                    EMBED_FILES "data/favicon.ico" "data/index.html" "data/style.css" "data/script.js")
