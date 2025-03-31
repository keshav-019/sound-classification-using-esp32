# components/my_module/CMakeLists.txt
idf_component_register(
    SRCS "src/i2s_recorder.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_driver_i2s fatfs
    INCLUDE_DIRS "$ENV{IDF_PATH}/examples/peripherals/i2s/common"
)