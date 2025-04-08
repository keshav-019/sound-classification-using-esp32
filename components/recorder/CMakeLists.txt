idf_component_register(SRCS "src/i2s_recorder_main.c"
                    PRIV_REQUIRES esp_driver_i2s fatfs
                    INCLUDE_DIRS "$ENV{IDF_PATH}/examples/peripherals/i2s/common" "include")
