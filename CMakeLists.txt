cmake_minimum_required(VERSION 3.24)

project(libvirtualdisplay
        VERSION 0.0.0
        DESCRIPTION "Clean-room virtual display driver support for Sunshine."
        HOMEPAGE_URL "https://github.com/Nonary/libvirtualdisplay"
        LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Choose the type of build." FORCE)
endif()

if(CMAKE_PROJECT_NAME STREQUAL PROJECT_NAME)
    option(BUILD_TESTS "Build tests" ON)
endif()

add_subdirectory(src)

if(CMAKE_PROJECT_NAME STREQUAL PROJECT_NAME AND BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
