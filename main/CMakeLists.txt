idf_component_register(SRCS "main.c" "mount.c"
                    INCLUDE_DIRS "." "/home/sumanshu/MtechProject/sound-classification-using-esp32-master/protocol_examples_common/include" "$ENV{IDF_PATH}/examples/peripherals/i2s/common"
                    PRIV_REQUIRES esp_driver_i2s fatfs spiffs esp_event esp_netif nvs_flash http_server esp_http_server model wifi_soft_access_point esp_wifi recorder file_operations
                    )