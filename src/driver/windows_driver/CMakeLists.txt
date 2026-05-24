if(NOT WIN32 OR NOT MSVC)
    message(FATAL_ERROR "BUILD_SUNSHINE_VIRTUAL_DISPLAY_DRIVER requires a Windows MSVC/WDK build environment")
endif()

set(SUNSHINE_DRIVER_TARGET SunshineVirtualDisplayDriver)
set(SUNSHINE_DRIVER_SDK_VERSION "${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}")

if(NOT SUNSHINE_DRIVER_SDK_VERSION)
    set(SUNSHINE_DRIVER_SDK_VERSION "$ENV{WindowsSDKVersion}")
    string(REGEX REPLACE "\\\\$" "" SUNSHINE_DRIVER_SDK_VERSION "${SUNSHINE_DRIVER_SDK_VERSION}")
endif()

if(NOT SUNSHINE_DRIVER_SDK_VERSION)
    message(FATAL_ERROR "Unable to determine Windows SDK version for ${SUNSHINE_DRIVER_TARGET}")
endif()

set(SUNSHINE_DRIVER_WDF_VERSION "2.25" CACHE STRING "UMDF version for the Sunshine virtual display driver" FORCE)
set(SUNSHINE_DRIVER_IDDCX_VERSION "1.10" CACHE STRING "IddCx include/library version for the Sunshine virtual display driver" FORCE)
set(SUNSHINE_DRIVER_ARCH "${CMAKE_VS_PLATFORM_NAME}")

if(NOT SUNSHINE_DRIVER_ARCH)
    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(SUNSHINE_DRIVER_ARCH "x64")
    else()
        set(SUNSHINE_DRIVER_ARCH "x86")
    endif()
endif()

set(SUNSHINE_DRIVER_WDK_ROOT "$ENV{WindowsSdkDir}")
if(NOT SUNSHINE_DRIVER_WDK_ROOT)
    set(SUNSHINE_DRIVER_WDK_ROOT "C:/Program Files (x86)/Windows Kits/10")
endif()

add_library(${SUNSHINE_DRIVER_TARGET} SHARED
        driver_main.cpp
        SunshineVirtualDisplayDriver.inf)

target_link_libraries(${SUNSHINE_DRIVER_TARGET}
        PRIVATE
        libvirtualdisplay::driver
        d3d11
        dxgi
        "${SUNSHINE_DRIVER_WDK_ROOT}/Lib/${SUNSHINE_DRIVER_SDK_VERSION}/um/${SUNSHINE_DRIVER_ARCH}/iddcx/${SUNSHINE_DRIVER_IDDCX_VERSION}/IddCxStub.lib"
        "${SUNSHINE_DRIVER_WDK_ROOT}/Lib/wdf/umdf/${SUNSHINE_DRIVER_ARCH}/${SUNSHINE_DRIVER_WDF_VERSION}/WdfDriverStubUm.lib"
        "${SUNSHINE_DRIVER_WDK_ROOT}/Lib/${SUNSHINE_DRIVER_SDK_VERSION}/um/${SUNSHINE_DRIVER_ARCH}/ntdll.lib")

target_include_directories(${SUNSHINE_DRIVER_TARGET}
        PRIVATE
        "${SUNSHINE_DRIVER_WDK_ROOT}/Include/wdf/umdf/${SUNSHINE_DRIVER_WDF_VERSION}"
        "${SUNSHINE_DRIVER_WDK_ROOT}/Include/${SUNSHINE_DRIVER_SDK_VERSION}/um"
        "${SUNSHINE_DRIVER_WDK_ROOT}/Include/${SUNSHINE_DRIVER_SDK_VERSION}/shared"
        "${SUNSHINE_DRIVER_WDK_ROOT}/Include/${SUNSHINE_DRIVER_SDK_VERSION}/ucrt"
        "${SUNSHINE_DRIVER_WDK_ROOT}/Include/${SUNSHINE_DRIVER_SDK_VERSION}/um/iddcx/${SUNSHINE_DRIVER_IDDCX_VERSION}")

target_compile_definitions(${SUNSHINE_DRIVER_TARGET}
        PRIVATE
        UNICODE
        _UNICODE
        NOMINMAX
        UMDF_DRIVER
        UMDF_VERSION_MAJOR=2
        UMDF_VERSION_MINOR=25
        UMDF_MINIMUM_VERSION_REQUIRED=25
        _ATL_NO_WIN_SUPPORT
        IDDCX_VERSION_MAJOR=1
        IDDCX_VERSION_MINOR=10
        IDDCX_MINIMUM_VERSION_REQUIRED=4)

set_target_properties(${SUNSHINE_DRIVER_TARGET}
        PROPERTIES
        PREFIX ""
        OUTPUT_NAME "SunshineVirtualDisplayDriver"
        SUFFIX ".dll")

add_custom_command(TARGET ${SUNSHINE_DRIVER_TARGET} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/SunshineVirtualDisplayDriver.inf"
        "$<TARGET_FILE_DIR:${SUNSHINE_DRIVER_TARGET}>/SunshineVirtualDisplayDriver.inf")
