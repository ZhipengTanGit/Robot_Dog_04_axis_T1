# For more information about build system see
# https://docs.espressif.com/projects/esp-idf/en/latest/api-guides/build-system.html
# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

set(PROJECT_VER "2.1.0")

# Add this line to disable the specific warning
add_compile_options(-Wno-missing-field-initializers)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(xiaozhi)

# This hardware port is validated only on the pinned ESP-IDF 5.4 release.
if(NOT IDF_VERSION_MAJOR EQUAL 5 OR NOT IDF_VERSION_MINOR EQUAL 4)
    message(FATAL_ERROR "Robot dog voice firmware requires ESP-IDF 5.4.x (validated: 5.4.4)")
endif()

