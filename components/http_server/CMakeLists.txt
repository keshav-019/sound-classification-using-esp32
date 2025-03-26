# components/my_module/CMakeLists.txt
idf_component_register(
    SRCS "src/https_server_hosting.c"
    INCLUDE_DIRS "include"
)