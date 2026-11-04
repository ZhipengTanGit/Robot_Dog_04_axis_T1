cmake_minimum_required(VERSION 3.16)

option(ROBOT_PUBLIC_TEST_BUILD "USB diagnostics without private bench configuration" OFF)
if(ROBOT_PUBLIC_TEST_BUILD)
    set(PROJECT_VER "public-usb-jpeg-small-20261102-r1")
endif()

set(EXTRA_COMPONENT_DIRS "Bsp")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
if(ROBOT_PUBLIC_TEST_BUILD)
    set(DEPENDENCIES_LOCK "${CMAKE_BINARY_DIR}/dependencies.lock")
    idf_build_set_property(DEPENDENCIES_LOCK "${DEPENDENCIES_LOCK}")
endif()
idf_build_set_property(MINIMAL_BUILD ON)
project(esp32_robot_dog_bridge)
