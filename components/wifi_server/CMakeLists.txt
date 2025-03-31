# components/my_module/CMakeLists.txt
idf_component_register(
    SRCS "src/station_mode_wifi.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_wifi nvs_flash
)