include(GoogleTest)

set(INSTALL_GTEST OFF)
set(INSTALL_GMOCK OFF)
set(gtest_force_shared_crt ON CACHE BOOL "Always use msvcrt.dll" FORCE)
set(LIBVIRTUALDISPLAY_GOOGLETEST_SOURCE_DIR "" CACHE PATH "Optional local googletest source directory")

if(LIBVIRTUALDISPLAY_GOOGLETEST_SOURCE_DIR AND EXISTS "${LIBVIRTUALDISPLAY_GOOGLETEST_SOURCE_DIR}/CMakeLists.txt")
    add_subdirectory("${LIBVIRTUALDISPLAY_GOOGLETEST_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/googletest")
else()
    include(FetchContent)
    FetchContent_Declare(
            googletest
            GIT_REPOSITORY https://github.com/google/googletest.git
            GIT_TAG main)
    FetchContent_MakeAvailable(googletest)
endif()

file(GLOB_RECURSE TEST_SOURCES CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/unit/*.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/unit/*.h")

add_executable(test_libvirtualdisplay ${TEST_SOURCES})
target_link_libraries(test_libvirtualdisplay
        PRIVATE
        gmock_main
        libvirtualdisplay::driver)
target_compile_definitions(test_libvirtualdisplay
        PRIVATE
        LIBVIRTUALDISPLAY_SOURCE_DIR="${PROJECT_SOURCE_DIR}")

gtest_discover_tests(test_libvirtualdisplay)
