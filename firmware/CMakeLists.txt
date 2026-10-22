# remote-wake firmware — ESP-IDF >= 5.2
cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../components")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

if(NOT DEFINED PROJECT_VER)
    set(PROJECT_VER "1.1.0")
endif()
project(remote-wake)
