# components/my_module/CMakeLists.txt
idf_component_register(
    SRCS "src/https_server_hosting.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES vfs spiffs esp_https_server esp_wifi nvs_flash
    EMBED_FILES "data/servercert.pem" 
                 "data/privkey.pem" 
                 "data/script.js" 
                 "data/style.css"
)