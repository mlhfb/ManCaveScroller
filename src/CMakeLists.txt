# This file was automatically generated for projects
# without default 'CMakeLists.txt' file.

FILE(GLOB_RECURSE app_sources ${CMAKE_SOURCE_DIR}/src/*.*)

idf_component_register(
    SRCS ${app_sources}
    INCLUDE_DIRS "${CMAKE_SOURCE_DIR}/include"
    REQUIRES driver nvs_flash esp_wifi esp_http_server esp_http_client esp_event esp_netif lwip esp_littlefs esp-idf-fastled
)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)

littlefs_create_partition_image(littlefs ${CMAKE_SOURCE_DIR}/littlefs FLASH_IN_PROJECT)
