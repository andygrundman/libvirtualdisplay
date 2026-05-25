cmake_minimum_required(VERSION 3.24)

project(libvirtualdisplay
        VERSION 0.0.0
        DESCRIPTION "Clean-room virtual display driver support for Sunshine."
        HOMEPAGE_URL "https://github.com/Nonary/libvirtualdisplay"
        LANGUAGES CXX)

set(LIBVIRTUALDISPLAY_PACKAGE_VERSION "${PROJECT_VERSION}" CACHE STRING "Version used for generated release packages")

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

if(CMAKE_PROJECT_NAME STREQUAL PROJECT_NAME)
    install(FILES README.md LICENSE DESTINATION .)

    set(CPACK_PACKAGE_NAME "libvirtualdisplay")
    set(CPACK_PACKAGE_VENDOR "LizardByte")
    set(CPACK_PACKAGE_CONTACT "https://github.com/Nonary/libvirtualdisplay")
    set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "${PROJECT_DESCRIPTION}")
    set(CPACK_PACKAGE_VERSION "${LIBVIRTUALDISPLAY_PACKAGE_VERSION}")
    set(CPACK_GENERATOR "ZIP")
    set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY ON)
    set(CPACK_PACKAGE_FILE_NAME "${CPACK_PACKAGE_NAME}-${CPACK_PACKAGE_VERSION}-windows-x64")
    include(CPack)
endif()
