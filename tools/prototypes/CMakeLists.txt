cmake_minimum_required(VERSION 3.25)
project(VirtualPostgreSQLStage0Prototypes LANGUAGES C)

if(NOT MSVC OR NOT WIN32)
    message(FATAL_ERROR "Stage 0 prototype runtime contour currently requires MSVC on Windows")
endif()
if(NOT DEFINED VPS_DEPS_ROOT OR NOT IS_DIRECTORY "${VPS_DEPS_ROOT}")
    message(FATAL_ERROR "VPS_DEPS_ROOT must name one architecture/configuration dependency root")
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

function(vps_import_static target path)
    if(NOT EXISTS "${path}")
        message(FATAL_ERROR "Missing static dependency: ${path}")
    endif()
    add_library(${target} STATIC IMPORTED GLOBAL)
    set_target_properties(${target} PROPERTIES IMPORTED_LOCATION "${path}")
endfunction()

set(VPS_LIBPQ_ROOT "${VPS_DEPS_ROOT}/libpq")
set(VPS_OPENSSL_ROOT "${VPS_DEPS_ROOT}/openssl")
set(VPS_ZLIB_ROOT "${VPS_DEPS_ROOT}/zlib")
vps_import_static(vps_libpq "${VPS_LIBPQ_ROOT}/lib/libpq.lib")
vps_import_static(vps_pgcommon "${VPS_LIBPQ_ROOT}/lib/pgcommon.lib")
vps_import_static(vps_pgport "${VPS_LIBPQ_ROOT}/lib/pgport.lib")
vps_import_static(vps_ssl "${VPS_OPENSSL_ROOT}/lib/libssl.lib")
vps_import_static(vps_crypto "${VPS_OPENSSL_ROOT}/lib/libcrypto.lib")
vps_import_static(vps_zlib "${VPS_ZLIB_ROOT}/lib/zlib.lib")

add_library(vps_proto_common STATIC vps_proto_common.c)
target_compile_definitions(vps_proto_common PRIVATE _CRT_SECURE_NO_WARNINGS)
target_include_directories(vps_proto_common PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${VPS_LIBPQ_ROOT}/include"
    "${VPS_OPENSSL_ROOT}/include"
    "${VPS_ZLIB_ROOT}/include"
)
target_compile_options(vps_proto_common PRIVATE /W4 /WX /sdl /TC)

add_executable(vps_proto_connect vps_proto_connect.c)
target_compile_definitions(vps_proto_connect PRIVATE _CRT_SECURE_NO_WARNINGS)
target_compile_options(vps_proto_connect PRIVATE /W4 /WX /sdl /TC)
target_link_libraries(vps_proto_connect PRIVATE
    vps_proto_common
    vps_libpq vps_pgcommon vps_pgport
    vps_ssl vps_crypto vps_zlib
    ws2_32 secur32 crypt32 advapi32 user32 shell32
)

add_executable(vps_proto_query vps_proto_query.c)
target_compile_definitions(vps_proto_query PRIVATE _CRT_SECURE_NO_WARNINGS)
target_compile_options(vps_proto_query PRIVATE /W4 /WX /sdl /TC)
target_link_libraries(vps_proto_query PRIVATE
    vps_proto_common
    vps_libpq vps_pgcommon vps_pgport
    vps_ssl vps_crypto vps_zlib
    ws2_32 secur32 crypt32 advapi32 user32 shell32
)

add_executable(vps_proto_cancel vps_proto_cancel.c)
target_compile_definitions(vps_proto_cancel PRIVATE _CRT_SECURE_NO_WARNINGS)
target_compile_options(vps_proto_cancel PRIVATE /W4 /WX /sdl /TC)
target_link_libraries(vps_proto_cancel PRIVATE
    vps_proto_common
    vps_libpq vps_pgcommon vps_pgport
    vps_ssl vps_crypto vps_zlib
    ws2_32 secur32 crypt32 advapi32 user32 shell32
)

add_executable(vps_proto_concurrency vps_proto_concurrency.c)
target_compile_definitions(vps_proto_concurrency PRIVATE _CRT_SECURE_NO_WARNINGS)
target_compile_options(vps_proto_concurrency PRIVATE /W4 /WX /sdl /TC)
target_link_libraries(vps_proto_concurrency PRIVATE
    vps_proto_common
    vps_libpq vps_pgcommon vps_pgport
    vps_ssl vps_crypto vps_zlib
    ws2_32 secur32 crypt32 advapi32 user32 shell32 psapi
)

add_executable(vps_proto_transaction vps_proto_transaction.c)
target_compile_definitions(vps_proto_transaction PRIVATE _CRT_SECURE_NO_WARNINGS)
target_compile_options(vps_proto_transaction PRIVATE /W4 /WX /sdl /TC)
target_link_libraries(vps_proto_transaction PRIVATE
    vps_proto_common
    vps_libpq vps_pgcommon vps_pgport
    vps_ssl vps_crypto vps_zlib
    ws2_32 secur32 crypt32 advapi32 user32 shell32
)

add_executable(vps_proto_postgis vps_proto_postgis.c)
target_compile_definitions(vps_proto_postgis PRIVATE _CRT_SECURE_NO_WARNINGS)
target_compile_options(vps_proto_postgis PRIVATE /W4 /WX /sdl /TC)
target_link_libraries(vps_proto_postgis PRIVATE
    vps_proto_common
    vps_libpq vps_pgcommon vps_pgport
    vps_ssl vps_crypto vps_zlib
    ws2_32 secur32 crypt32 advapi32 user32 shell32
)

add_executable(vps_proto_stream vps_proto_stream.c)
target_compile_definitions(vps_proto_stream PRIVATE _CRT_SECURE_NO_WARNINGS)
target_compile_options(vps_proto_stream PRIVATE /W4 /WX /sdl /TC)
target_link_libraries(vps_proto_stream PRIVATE
    vps_proto_common
    vps_libpq vps_pgcommon vps_pgport
    vps_ssl vps_crypto vps_zlib
    ws2_32 secur32 crypt32 advapi32 user32 shell32 psapi
)
