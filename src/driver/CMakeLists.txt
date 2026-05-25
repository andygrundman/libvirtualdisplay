set(MODULE libvirtualdisplay_driver)
set(MODULE_ALIAS libvirtualdisplay::driver)

file(GLOB HEADER_LIST CONFIGURE_DEPENDS "include/virtual_display/driver/*.h")
file(GLOB SOURCE_LIST CONFIGURE_DEPENDS "*.cpp")

add_library(${MODULE} ${HEADER_LIST} ${SOURCE_LIST})
add_library(${MODULE_ALIAS} ALIAS ${MODULE})

target_include_directories(${MODULE} PUBLIC include)
target_compile_features(${MODULE} PUBLIC cxx_std_20)

if(WIN32)
    target_link_libraries(${MODULE} PRIVATE setupapi)
endif()

option(BUILD_SUNSHINE_VIRTUAL_DISPLAY_DRIVER "Build the Windows UMDF/IddCx Sunshine virtual display driver" OFF)
if(CMAKE_PROJECT_NAME STREQUAL PROJECT_NAME)
    set(_libvirtualdisplay_probe_default ON)
else()
    set(_libvirtualdisplay_probe_default OFF)
endif()
option(BUILD_VIRTUALDISPLAY_PROBE "Build virtual display driver probe executable" ${_libvirtualdisplay_probe_default})
unset(_libvirtualdisplay_probe_default)

if(BUILD_SUNSHINE_VIRTUAL_DISPLAY_DRIVER)
    add_subdirectory(windows_driver)
endif()

if(WIN32 AND BUILD_VIRTUALDISPLAY_PROBE)
    add_executable(virtualdisplay_probe "${PROJECT_SOURCE_DIR}/tools/virtualdisplay_probe.cpp")
    target_link_libraries(virtualdisplay_probe PRIVATE libvirtualdisplay::driver)
    target_compile_features(virtualdisplay_probe PRIVATE cxx_std_20)
    install(TARGETS virtualdisplay_probe RUNTIME DESTINATION tools)

    add_executable(virtualdisplay "${PROJECT_SOURCE_DIR}/tools/virtualdisplay.cpp")
    target_link_libraries(virtualdisplay PRIVATE libvirtualdisplay::driver shell32 newdev)
    target_compile_features(virtualdisplay PRIVATE cxx_std_20)
    install(TARGETS virtualdisplay RUNTIME DESTINATION tools)

    add_executable(virtualdisplay_broker "${PROJECT_SOURCE_DIR}/tools/virtualdisplay_broker.cpp")
    target_link_libraries(virtualdisplay_broker PRIVATE libvirtualdisplay::driver advapi32 userenv wtsapi32)
    target_compile_features(virtualdisplay_broker PRIVATE cxx_std_20)
    install(TARGETS virtualdisplay_broker RUNTIME DESTINATION tools)
endif()
