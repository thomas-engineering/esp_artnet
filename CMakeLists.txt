cmake_minimum_required(VERSION 3.16)

set(IDF_TARGET "esp32s3")

file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/version.txt" PROJECT_VER LIMIT_COUNT 1)
string(STRIP "${PROJECT_VER}" PROJECT_VER)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp_artnet)
