idf_component_register(SRCS "src/file_operations.c"
                    INCLUDE_DIRS "include"
                    PRIV_REQUIRES fatfs recorder esp_driver_i2s)
