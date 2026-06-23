cmake_minimum_required(VERSION 3.20)

project(SyncHelperDemo LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT MSVC)
    message(FATAL_ERROR "This project is Windows/MSVC only.")
endif()

set(FBRO_DEPS "${CMAKE_SOURCE_DIR}/third_party/fbro" CACHE PATH "Local FBro dependency root" FORCE)
set(FBRO_INCLUDE "${FBRO_DEPS}/FBrowserCEF3lib")
set(FBRO_LIB_DIR "${FBRO_DEPS}/lib64")
set(FBRO_RUNTIME_DIR "${FBRO_DEPS}/CEFLib64")

add_executable(SyncHelperDemo WIN32
    src/main.cpp
)

target_compile_definitions(SyncHelperDemo PRIVATE
    WIN32
    _WIN32
    _WINDOWS
    UNICODE
    _UNICODE
    WIN64
    _WIN64
)

target_include_directories(SyncHelperDemo PRIVATE
    "${FBRO_INCLUDE}"
)

target_link_libraries(SyncHelperDemo PRIVATE
    "${FBRO_LIB_DIR}/$<$<CONFIG:Debug>:debug>$<$<NOT:$<CONFIG:Debug>>:release>/libcef_dll_wrapper.lib"
    "${FBRO_LIB_DIR}/FBrowserCEF3lib.lib"
    "${FBRO_LIB_DIR}/FBrowserVIP.lib"
    "${FBRO_LIB_DIR}/libcef.lib"
    kernel32.lib
    user32.lib
    gdi32.lib
    advapi32.lib
    shell32.lib
    ole32.lib
    oleaut32.lib
    ws2_32.lib
    uuid.lib
    comctl32.lib
)

set_property(TARGET SyncHelperDemo PROPERTY
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
)

target_link_options(SyncHelperDemo PRIVATE
    /ignore:4099
    /SUBSYSTEM:WINDOWS
)

add_custom_command(TARGET SyncHelperDemo POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${FBRO_RUNTIME_DIR}"
        "$<TARGET_FILE_DIR:SyncHelperDemo>"
    COMMENT "Copying FBro/CEF runtime files"
)
