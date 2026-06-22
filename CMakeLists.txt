cmake_minimum_required(VERSION 3.15)
project(klipper_status C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# lv_conf.h lives next to this file and is included as <lv_conf.h>.
add_compile_definitions(LV_CONF_INCLUDE_SIMPLE)
include_directories(${CMAKE_CURRENT_SOURCE_DIR})

# LVGL (cloned into ./lvgl by build.sh) and the vendored single-file cJSON.
add_subdirectory(lvgl)

# No external libraries: HTTP is done over a raw socket, JSON via vendored
# cJSON. Only libc/libm/pthread are needed, which exist on any Linux target.
find_package(Threads REQUIRED)

add_executable(klipper_status
    main.c
    ui.c
    moonraker.c
    third_party/cJSON/cJSON.c
    third_party/lodepng/lodepng.c
)

target_include_directories(klipper_status PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/cJSON
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/lodepng
)

target_link_libraries(klipper_status PRIVATE
    lvgl
    Threads::Threads
)
