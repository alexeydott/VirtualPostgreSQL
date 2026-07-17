cmake_minimum_required(VERSION 3.25)

project(
  VirtualPostgreSQL
  VERSION 0.1.0
  DESCRIPTION "Portable SQLite Virtual Table extension for PostgreSQL"
  LANGUAGES C
)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

set(CMAKE_MSVC_RUNTIME_LIBRARY
  "MultiThreaded$<$<CONFIG:Debug>:Debug>"
  CACHE STRING "Use the static MSVC runtime" FORCE)

function(vps_apply_strict_warnings target_name)
  if(MSVC)
    target_compile_options(${target_name} PRIVATE
      /W4 /WX /permissive- /utf-8 /sdl)
  elseif(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(${target_name} PRIVATE
      -Wall -Wextra -Wpedantic -Werror -fvisibility=hidden)
  endif()
endfunction()

set(VPS_SQLITE_AMALGAMATION_DIR
  "${CMAKE_CURRENT_SOURCE_DIR}/build/deps/sqlite-3.53.3/sqlite-amalgamation-3530300"
  CACHE PATH "Directory containing pinned sqlite3.c/sqlite3.h/sqlite3ext.h")
option(VPS_BUILD_EXTENSION "Build the loadable extension and SQLite host tests" ON)
option(VPS_ENABLE_ASAN "Enable AddressSanitizer for supported Windows targets" OFF)
option(VPS_ENABLE_WINCRED "Build the Windows Credential Manager adapter" ON)

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
  set(VPS_DEPENDENCY_ARCH "x64")
else()
  set(VPS_DEPENDENCY_ARCH "x86")
endif()
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
  set(VPS_DEPENDENCY_CONFIG "Debug")
else()
  set(VPS_DEPENDENCY_CONFIG "Release")
endif()
set(VPS_PINNED_DEPENDENCY_ROOT
  "${CMAKE_CURRENT_SOURCE_DIR}/build/deps/${VPS_DEPENDENCY_ARCH}/${VPS_DEPENDENCY_CONFIG}")
set(VPS_LIBPQ_INCLUDE_DIR "${VPS_PINNED_DEPENDENCY_ROOT}/libpq/include")
set(VPS_LIBPQ_LIBRARY_DIR "${VPS_PINNED_DEPENDENCY_ROOT}/libpq/lib")
set(VPS_OPENSSL_LIBRARY_DIR "${VPS_PINNED_DEPENDENCY_ROOT}/openssl/lib")
set(VPS_ZLIB_LIBRARY_DIR "${VPS_PINNED_DEPENDENCY_ROOT}/zlib/lib")

if(VPS_ENABLE_ASAN)
  if(NOT (MSVC AND CMAKE_C_COMPILER_ID MATCHES "Clang"))
    message(FATAL_ERROR
      "[sanitizer] VPS_ENABLE_ASAN requires clang-cl on Windows")
  endif()
  if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR
      "[sanitizer] the current AddressSanitizer contour supports x64 only")
  endif()

  execute_process(
    COMMAND "${CMAKE_C_COMPILER}" -print-resource-dir
    OUTPUT_VARIABLE VPS_CLANG_RESOURCE_DIR
    OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE VPS_CLANG_RESOURCE_RESULT
  )
  if(NOT VPS_CLANG_RESOURCE_RESULT EQUAL 0)
    message(FATAL_ERROR
      "[sanitizer] unable to discover the clang resource directory")
  endif()

  set(VPS_CLANG_ASAN_DIR "${VPS_CLANG_RESOURCE_DIR}/lib/windows")
  set(VPS_CLANG_ASAN_RUNTIME
    "${VPS_CLANG_ASAN_DIR}/clang_rt.asan-x86_64.lib")
  set(VPS_CLANG_ASAN_CXX_RUNTIME
    "${VPS_CLANG_ASAN_DIR}/clang_rt.asan_cxx-x86_64.lib")
  foreach(vps_asan_file IN ITEMS
      "${VPS_CLANG_ASAN_RUNTIME}"
      "${VPS_CLANG_ASAN_CXX_RUNTIME}")
    if(NOT EXISTS "${vps_asan_file}")
      message(FATAL_ERROR
        "[sanitizer] required clang runtime is missing: ${vps_asan_file}")
    endif()
  endforeach()
endif()

function(vps_apply_asan target_name)
  if(NOT VPS_ENABLE_ASAN)
    return()
  endif()
  target_compile_options(${target_name} PRIVATE /fsanitize=address)

  get_target_property(vps_target_type ${target_name} TYPE)
  if(NOT vps_target_type STREQUAL "STATIC_LIBRARY")
    target_link_libraries(${target_name} PRIVATE
      "${VPS_CLANG_ASAN_RUNTIME}"
      "${VPS_CLANG_ASAN_CXX_RUNTIME}"
    )
    target_link_options(${target_name} PRIVATE
      "/wholearchive:${VPS_CLANG_ASAN_RUNTIME}"
      "/wholearchive:${VPS_CLANG_ASAN_CXX_RUNTIME}"
      /INFERASANLIBS:NO
    )
  endif()
endfunction()

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
  message(FATAL_ERROR
    "[layout] in-source builds are forbidden; choose an out-of-source build directory")
endif()

set(VPS_REQUIRED_SOURCE_DIRECTORIES
  deps
  docs
  examples
  include/virtualpostgresql
  licenses
  scripts
  src
  tests/fuzz
  tests/host
  tests/integration
  tests/performance
  tests/unit
  tools
)

foreach(vps_relative_directory IN LISTS VPS_REQUIRED_SOURCE_DIRECTORIES)
  if(NOT IS_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/${vps_relative_directory}")
    message(FATAL_ERROR
      "[layout] required source directory is missing: ${vps_relative_directory}")
  endif()
  message(STATUS "[layout] validated directory: ${vps_relative_directory}")
endforeach()

file(GLOB vps_src_children LIST_DIRECTORIES TRUE CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/src/*")
foreach(vps_src_child IN LISTS vps_src_children)
  if(IS_DIRECTORY "${vps_src_child}")
    file(RELATIVE_PATH vps_src_relative "${CMAKE_CURRENT_SOURCE_DIR}" "${vps_src_child}")
    message(FATAL_ERROR
      "[layout] src must remain flat; found directory: ${vps_src_relative}")
  endif()
endforeach()

message(STATUS "[layout] source layout validation passed")

add_library(vps_abi STATIC
  src/vps_abi.c
)
target_compile_features(vps_abi PUBLIC c_std_11)
set_target_properties(vps_abi PROPERTIES
  C_EXTENSIONS OFF
  POSITION_INDEPENDENT_CODE ON
)
target_include_directories(vps_abi
  PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
  PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)
vps_apply_strict_warnings(vps_abi)

add_library(vps_memory STATIC
  src/vps_memory.c
)
target_compile_features(vps_memory PUBLIC c_std_11)
set_target_properties(vps_memory PROPERTIES
  C_EXTENSIONS OFF
  POSITION_INDEPENDENT_CODE ON
)
target_include_directories(vps_memory PUBLIC
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>"
)
vps_apply_strict_warnings(vps_memory)
vps_apply_asan(vps_memory)

add_library(vps_error STATIC
  src/vps_error.c
)
target_compile_features(vps_error PUBLIC c_std_11)
set_target_properties(vps_error PROPERTIES
  C_EXTENSIONS OFF
  POSITION_INDEPENDENT_CODE ON
)
target_include_directories(vps_error
  PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>"
  PRIVATE
    "${VPS_SQLITE_AMALGAMATION_DIR}"
)
target_link_libraries(vps_error PUBLIC vps_memory)
vps_apply_strict_warnings(vps_error)
vps_apply_asan(vps_error)

add_library(vps_logging STATIC
  src/vps_logging.c
)
target_compile_features(vps_logging PUBLIC c_std_11)
set_target_properties(vps_logging PROPERTIES
  C_EXTENSIONS OFF
  POSITION_INDEPENDENT_CODE ON
)
target_include_directories(vps_logging PUBLIC
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>"
)
vps_apply_strict_warnings(vps_logging)
vps_apply_asan(vps_logging)

add_library(vps_deadline STATIC
  src/vps_deadline.c
)
target_compile_features(vps_deadline PUBLIC c_std_11)
set_target_properties(vps_deadline PROPERTIES
  C_EXTENSIONS OFF
  POSITION_INDEPENDENT_CODE ON
)
target_include_directories(vps_deadline PUBLIC
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>"
)
target_link_libraries(vps_deadline PUBLIC vps_logging vps_platform)
vps_apply_strict_warnings(vps_deadline)
vps_apply_asan(vps_deadline)

add_library(vps_platform STATIC
  src/vps_platform.c
  src/vps_android.c
  src/vps_posix.c
)
if(WIN32)
  target_sources(vps_platform PRIVATE src/vps_windows.c)
endif()
target_compile_features(vps_platform PUBLIC c_std_11)
set_target_properties(vps_platform PROPERTIES
  C_EXTENSIONS OFF
  POSITION_INDEPENDENT_CODE ON
)
target_include_directories(vps_platform PUBLIC
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>"
)
if(WIN32)
  target_link_libraries(vps_platform PRIVATE bcrypt ws2_32)
endif()
vps_apply_strict_warnings(vps_platform)

add_library(vps_secure_memory STATIC
  src/vps_secure_memory.c
)
target_compile_features(vps_secure_memory PUBLIC c_std_11)
set_target_properties(vps_secure_memory PROPERTIES
  C_EXTENSIONS OFF
  POSITION_INDEPENDENT_CODE ON
)
target_include_directories(vps_secure_memory PUBLIC
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>"
)
target_link_libraries(vps_secure_memory PUBLIC
  vps_logging
  vps_memory
  vps_platform
)
vps_apply_strict_warnings(vps_secure_memory)
vps_apply_asan(vps_secure_memory)

add_library(vps_arguments STATIC
  src/vps_arguments.c
)
target_compile_features(vps_arguments PUBLIC c_std_11)
set_target_properties(vps_arguments PROPERTIES
  C_EXTENSIONS OFF
  POSITION_INDEPENDENT_CODE ON
)
target_include_directories(vps_arguments PUBLIC
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>"
)
target_link_libraries(vps_arguments PUBLIC
  vps_logging
  vps_memory
  vps_platform
  vps_secure_memory
)
vps_apply_strict_warnings(vps_arguments)
vps_apply_asan(vps_arguments)

add_library(vps_credential_provider STATIC
  src/vps_credential_provider.c
)
target_compile_features(vps_credential_provider PUBLIC c_std_11)
set_target_properties(vps_credential_provider PROPERTIES
  C_EXTENSIONS OFF
  POSITION_INDEPENDENT_CODE ON
)
target_include_directories(vps_credential_provider PUBLIC
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>"
)
target_link_libraries(vps_credential_provider PUBLIC
  vps_abi
  vps_logging
  vps_memory
  vps_platform
  vps_secure_memory
)
vps_apply_strict_warnings(vps_credential_provider)
vps_apply_asan(vps_credential_provider)

add_library(vps_connection_string STATIC
  src/vps_connection_string.c
)
target_compile_features(vps_connection_string PUBLIC c_std_11)
set_target_properties(vps_connection_string PROPERTIES
  C_EXTENSIONS OFF
  POSITION_INDEPENDENT_CODE ON
)
target_include_directories(vps_connection_string PUBLIC
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>"
)
target_link_libraries(vps_connection_string PUBLIC
  vps_arguments
  vps_credential_provider
)
vps_apply_strict_warnings(vps_connection_string)
vps_apply_asan(vps_connection_string)

add_library(vps_identity STATIC
  src/vps_identity.c
)
target_compile_features(vps_identity PUBLIC c_std_11)
set_target_properties(vps_identity PROPERTIES
  C_EXTENSIONS OFF
  POSITION_INDEPENDENT_CODE ON
)
target_include_directories(vps_identity PUBLIC
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>"
)
target_link_libraries(vps_identity PUBLIC vps_connection_string)
vps_apply_strict_warnings(vps_identity)
vps_apply_asan(vps_identity)

add_library(vps_tls_policy STATIC
  src/vps_tls_policy.c
)
target_compile_features(vps_tls_policy PUBLIC c_std_11)
set_target_properties(vps_tls_policy PROPERTIES
  C_EXTENSIONS OFF
  POSITION_INDEPENDENT_CODE ON
)
target_include_directories(vps_tls_policy PUBLIC
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>"
)
target_link_libraries(vps_tls_policy PUBLIC vps_logging)
vps_apply_strict_warnings(vps_tls_policy)
vps_apply_asan(vps_tls_policy)

add_library(vps_session STATIC
  src/vps_session.c
)
target_compile_features(vps_session PUBLIC c_std_11)
set_target_properties(vps_session PROPERTIES
  C_EXTENSIONS OFF
  POSITION_INDEPENDENT_CODE ON
)
target_include_directories(vps_session PUBLIC
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>"
)
target_link_libraries(vps_session PUBLIC
  vps_connection_string
  vps_logging
)
vps_apply_strict_warnings(vps_session)
vps_apply_asan(vps_session)

if(EXISTS "${VPS_LIBPQ_INCLUDE_DIR}/libpq-fe.h" AND
   EXISTS "${VPS_LIBPQ_LIBRARY_DIR}/libpq.lib")
  add_library(vps_libpq_client_conninfo STATIC
    src/vps_libpq_client_conninfo.c
  )
  target_compile_features(vps_libpq_client_conninfo PUBLIC c_std_11)
  set_target_properties(vps_libpq_client_conninfo PROPERTIES
    C_EXTENSIONS OFF
    POSITION_INDEPENDENT_CODE ON
  )
  target_include_directories(vps_libpq_client_conninfo
    PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>"
    PRIVATE "${VPS_LIBPQ_INCLUDE_DIR}"
  )
  target_link_libraries(vps_libpq_client_conninfo
    PUBLIC vps_connection_string
    PRIVATE
      "${VPS_LIBPQ_LIBRARY_DIR}/libpq.lib"
      "${VPS_LIBPQ_LIBRARY_DIR}/pgcommon.lib"
      "${VPS_LIBPQ_LIBRARY_DIR}/pgport.lib"
      "${VPS_OPENSSL_LIBRARY_DIR}/libssl.lib"
      "${VPS_OPENSSL_LIBRARY_DIR}/libcrypto.lib"
      "${VPS_ZLIB_LIBRARY_DIR}/zlib.lib"
      ws2_32 secur32 crypt32 advapi32 user32 shell32
  )
  vps_apply_strict_warnings(vps_libpq_client_conninfo)
  vps_apply_asan(vps_libpq_client_conninfo)

  add_library(vps_libpq_client_tls STATIC
    src/vps_libpq_client_tls.c
  )
  target_compile_features(vps_libpq_client_tls PUBLIC c_std_11)
  set_target_properties(vps_libpq_client_tls PROPERTIES
    C_EXTENSIONS OFF
    POSITION_INDEPENDENT_CODE ON
  )
  target_include_directories(vps_libpq_client_tls
    PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>"
    PRIVATE "${VPS_LIBPQ_INCLUDE_DIR}"
  )
  target_link_libraries(vps_libpq_client_tls
    PUBLIC vps_tls_policy
    PRIVATE
      "${VPS_LIBPQ_LIBRARY_DIR}/libpq.lib"
      "${VPS_LIBPQ_LIBRARY_DIR}/pgcommon.lib"
      "${VPS_LIBPQ_LIBRARY_DIR}/pgport.lib"
      "${VPS_OPENSSL_LIBRARY_DIR}/libssl.lib"
      "${VPS_OPENSSL_LIBRARY_DIR}/libcrypto.lib"
      "${VPS_ZLIB_LIBRARY_DIR}/zlib.lib"
      ws2_32 secur32 crypt32 advapi32 user32 shell32
  )
  vps_apply_strict_warnings(vps_libpq_client_tls)
  vps_apply_asan(vps_libpq_client_tls)

  add_library(vps_libpq_client_session STATIC
    src/vps_libpq_client_session.c
  )
  target_compile_features(vps_libpq_client_session PUBLIC c_std_11)
  set_target_properties(vps_libpq_client_session PROPERTIES
    C_EXTENSIONS OFF
    POSITION_INDEPENDENT_CODE ON
  )
  target_include_directories(vps_libpq_client_session
    PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>"
    PRIVATE "${VPS_LIBPQ_INCLUDE_DIR}"
  )
  target_link_libraries(vps_libpq_client_session
    PUBLIC vps_session
    PRIVATE
      "${VPS_LIBPQ_LIBRARY_DIR}/libpq.lib"
      "${VPS_LIBPQ_LIBRARY_DIR}/pgcommon.lib"
      "${VPS_LIBPQ_LIBRARY_DIR}/pgport.lib"
      "${VPS_OPENSSL_LIBRARY_DIR}/libssl.lib"
      "${VPS_OPENSSL_LIBRARY_DIR}/libcrypto.lib"
      "${VPS_ZLIB_LIBRARY_DIR}/zlib.lib"
      ws2_32 secur32 crypt32 advapi32 user32 shell32
  )
  vps_apply_strict_warnings(vps_libpq_client_session)
  vps_apply_asan(vps_libpq_client_session)
else()
  message(FATAL_ERROR
    "[libpq] pinned static dependency contour is incomplete: ${VPS_PINNED_DEPENDENCY_ROOT}")
endif()

if(WIN32 AND VPS_ENABLE_WINCRED)
  add_library(vps_wincred_provider STATIC
    src/vps_wincred_provider.c
  )
  target_compile_features(vps_wincred_provider PUBLIC c_std_11)
  set_target_properties(vps_wincred_provider PROPERTIES
    C_EXTENSIONS OFF
    POSITION_INDEPENDENT_CODE ON
  )
  target_include_directories(vps_wincred_provider PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>"
  )
  target_link_libraries(vps_wincred_provider PUBLIC
    vps_credential_provider
    PRIVATE advapi32
  )
  vps_apply_strict_warnings(vps_wincred_provider)
  vps_apply_asan(vps_wincred_provider)
endif()

if(VPS_BUILD_EXTENSION AND EXISTS "${VPS_SQLITE_AMALGAMATION_DIR}/sqlite3ext.h")
  add_library(virtualpostgresql MODULE
    src/vps_extension.c
    src/vps_module.c
  )
  target_compile_features(virtualpostgresql PRIVATE c_std_11)
  set_target_properties(virtualpostgresql PROPERTIES
    C_EXTENSIONS OFF
    PREFIX ""
    OUTPUT_NAME "virtualpostgresql"
  )
  target_compile_definitions(virtualpostgresql PRIVATE VPS_BUILD_DLL)
  target_include_directories(virtualpostgresql PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${VPS_SQLITE_AMALGAMATION_DIR}"
  )
  target_link_libraries(virtualpostgresql PRIVATE vps_abi)
  vps_apply_strict_warnings(virtualpostgresql)
  if(WIN32)
    target_sources(virtualpostgresql PRIVATE
      src/virtualpostgresql.def
      src/virtualpostgresql.rc
    )
  endif()
elseif(VPS_BUILD_EXTENSION)
  message(STATUS
    "[sqlite] pinned amalgamation unavailable; extension/host targets disabled")
endif()

include(CTest)
if(BUILD_TESTING)
  add_executable(vps_abi_layout_test
    tests/unit/vps_abi_layout_test.c
  )
  target_compile_features(vps_abi_layout_test PRIVATE c_std_11)
  set_target_properties(vps_abi_layout_test PROPERTIES C_EXTENSIONS OFF)
  target_include_directories(vps_abi_layout_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
  )
  target_link_libraries(vps_abi_layout_test PRIVATE vps_abi)
  vps_apply_strict_warnings(vps_abi_layout_test)
  add_test(NAME vps_abi_layout COMMAND vps_abi_layout_test)

  add_executable(vps_platform_contract_test
    tests/unit/vps_platform_contract_test.c
  )
  target_compile_features(vps_platform_contract_test PRIVATE c_std_11)
  set_target_properties(vps_platform_contract_test PROPERTIES C_EXTENSIONS OFF)
  target_link_libraries(vps_platform_contract_test PRIVATE vps_platform)
  vps_apply_strict_warnings(vps_platform_contract_test)
  add_test(NAME vps_platform_contract COMMAND vps_platform_contract_test)

  add_executable(vps_memory_test
    tests/unit/vps_memory_test.c
  )
  target_compile_features(vps_memory_test PRIVATE c_std_11)
  set_target_properties(vps_memory_test PROPERTIES C_EXTENSIONS OFF)
  target_link_libraries(vps_memory_test PRIVATE vps_memory)
  vps_apply_strict_warnings(vps_memory_test)
  vps_apply_asan(vps_memory_test)
  add_test(NAME vps_memory COMMAND vps_memory_test)

  add_executable(vps_error_test
    tests/unit/vps_error_test.c
  )
  target_compile_features(vps_error_test PRIVATE c_std_11)
  set_target_properties(vps_error_test PROPERTIES C_EXTENSIONS OFF)
  target_include_directories(vps_error_test PRIVATE
    "${VPS_SQLITE_AMALGAMATION_DIR}"
  )
  target_link_libraries(vps_error_test PRIVATE vps_error)
  vps_apply_strict_warnings(vps_error_test)
  vps_apply_asan(vps_error_test)
  add_test(NAME vps_error COMMAND vps_error_test)

  add_executable(vps_logging_test
    tests/unit/vps_logging_test.c
  )
  target_compile_features(vps_logging_test PRIVATE c_std_11)
  set_target_properties(vps_logging_test PROPERTIES C_EXTENSIONS OFF)
  target_link_libraries(vps_logging_test PRIVATE vps_logging)
  vps_apply_strict_warnings(vps_logging_test)
  vps_apply_asan(vps_logging_test)
  add_test(NAME vps_logging COMMAND vps_logging_test)

  add_executable(vps_deadline_test
    tests/unit/vps_deadline_test.c
  )
  target_compile_features(vps_deadline_test PRIVATE c_std_11)
  set_target_properties(vps_deadline_test PROPERTIES C_EXTENSIONS OFF)
  target_link_libraries(vps_deadline_test PRIVATE vps_deadline)
  vps_apply_strict_warnings(vps_deadline_test)
  vps_apply_asan(vps_deadline_test)
  add_test(NAME vps_deadline COMMAND vps_deadline_test)

  add_executable(vps_support_integration_test
    tests/integration/vps_support_integration_test.c
  )
  target_compile_features(vps_support_integration_test PRIVATE c_std_11)
  set_target_properties(vps_support_integration_test PROPERTIES
    C_EXTENSIONS OFF
  )
  target_compile_definitions(vps_support_integration_test PRIVATE
    $<$<CONFIG:Release>:VPS_OPTIMIZED_BUILD>
  )
  target_include_directories(vps_support_integration_test PRIVATE
    "${VPS_SQLITE_AMALGAMATION_DIR}"
  )
  target_link_libraries(vps_support_integration_test PRIVATE
    vps_deadline
    vps_error
    vps_secure_memory
  )
  vps_apply_strict_warnings(vps_support_integration_test)
  vps_apply_asan(vps_support_integration_test)
  add_test(NAME vps_support_integration COMMAND vps_support_integration_test)

  add_executable(vps_arguments_test
    tests/unit/vps_arguments_test.c
  )
  target_compile_features(vps_arguments_test PRIVATE c_std_11)
  set_target_properties(vps_arguments_test PROPERTIES C_EXTENSIONS OFF)
  target_link_libraries(vps_arguments_test PRIVATE vps_arguments)
  vps_apply_strict_warnings(vps_arguments_test)
  vps_apply_asan(vps_arguments_test)
  add_test(NAME vps_arguments COMMAND vps_arguments_test)

  add_executable(vps_credential_provider_test
    tests/unit/vps_credential_provider_test.c
  )
  target_compile_features(vps_credential_provider_test PRIVATE c_std_11)
  set_target_properties(vps_credential_provider_test PROPERTIES
    C_EXTENSIONS OFF
  )
  target_link_libraries(vps_credential_provider_test PRIVATE
    vps_credential_provider
  )
  vps_apply_strict_warnings(vps_credential_provider_test)
  vps_apply_asan(vps_credential_provider_test)
  add_test(NAME vps_credential_provider COMMAND vps_credential_provider_test)

  add_executable(vps_connection_string_test
    tests/unit/vps_connection_string_test.c
  )
  target_compile_features(vps_connection_string_test PRIVATE c_std_11)
  set_target_properties(vps_connection_string_test PROPERTIES C_EXTENSIONS OFF)
  target_link_libraries(vps_connection_string_test PRIVATE
    vps_connection_string
    vps_libpq_client_conninfo
  )
  vps_apply_strict_warnings(vps_connection_string_test)
  vps_apply_asan(vps_connection_string_test)
  add_test(NAME vps_connection_string COMMAND vps_connection_string_test)

  add_executable(vps_identity_test
    tests/unit/vps_identity_test.c
  )
  target_compile_features(vps_identity_test PRIVATE c_std_11)
  set_target_properties(vps_identity_test PROPERTIES C_EXTENSIONS OFF)
  target_link_libraries(vps_identity_test PRIVATE vps_identity)
  vps_apply_strict_warnings(vps_identity_test)
  vps_apply_asan(vps_identity_test)
  add_test(NAME vps_identity COMMAND vps_identity_test)

  add_executable(vps_tls_policy_test
    tests/unit/vps_tls_policy_test.c
  )
  target_compile_features(vps_tls_policy_test PRIVATE c_std_11)
  set_target_properties(vps_tls_policy_test PROPERTIES C_EXTENSIONS OFF)
  target_link_libraries(vps_tls_policy_test PRIVATE vps_tls_policy)
  vps_apply_strict_warnings(vps_tls_policy_test)
  vps_apply_asan(vps_tls_policy_test)
  add_test(NAME vps_tls_policy COMMAND vps_tls_policy_test)

  add_executable(vps_session_test
    tests/unit/vps_session_test.c
  )
  target_compile_features(vps_session_test PRIVATE c_std_11)
  set_target_properties(vps_session_test PROPERTIES C_EXTENSIONS OFF)
  target_link_libraries(vps_session_test PRIVATE vps_session)
  vps_apply_strict_warnings(vps_session_test)
  vps_apply_asan(vps_session_test)
  add_test(NAME vps_session COMMAND vps_session_test)

  add_executable(vps_libpq_client_tls_integration_test
    tests/integration/vps_libpq_client_tls_integration_test.c
  )
  target_compile_features(vps_libpq_client_tls_integration_test PRIVATE c_std_11)
  set_target_properties(vps_libpq_client_tls_integration_test PROPERTIES
    C_EXTENSIONS OFF
  )
  target_include_directories(vps_libpq_client_tls_integration_test PRIVATE
    "${VPS_LIBPQ_INCLUDE_DIR}"
  )
  if(MSVC)
    target_compile_definitions(vps_libpq_client_tls_integration_test PRIVATE
      _CRT_SECURE_NO_WARNINGS
    )
  endif()
  target_link_libraries(vps_libpq_client_tls_integration_test PRIVATE
    vps_libpq_client_tls
  )
  vps_apply_strict_warnings(vps_libpq_client_tls_integration_test)
  vps_apply_asan(vps_libpq_client_tls_integration_test)
  add_test(NAME vps_libpq_client_tls_integration
    COMMAND vps_libpq_client_tls_integration_test)
  set_tests_properties(vps_libpq_client_tls_integration PROPERTIES
    SKIP_RETURN_CODE 77
    LABELS "tls-integration"
  )

  add_executable(vps_libpq_client_session_integration_test
    tests/integration/vps_libpq_client_session_integration_test.c
  )
  target_compile_features(vps_libpq_client_session_integration_test PRIVATE
    c_std_11
  )
  set_target_properties(vps_libpq_client_session_integration_test PROPERTIES
    C_EXTENSIONS OFF
  )
  target_include_directories(vps_libpq_client_session_integration_test PRIVATE
    "${VPS_LIBPQ_INCLUDE_DIR}"
  )
  if(MSVC)
    target_compile_definitions(
      vps_libpq_client_session_integration_test PRIVATE
      _CRT_SECURE_NO_WARNINGS
    )
  endif()
  target_link_libraries(vps_libpq_client_session_integration_test PRIVATE
    vps_libpq_client_session
  )
  vps_apply_strict_warnings(vps_libpq_client_session_integration_test)
  vps_apply_asan(vps_libpq_client_session_integration_test)
  add_test(NAME vps_libpq_client_session_integration
    COMMAND vps_libpq_client_session_integration_test)
  set_tests_properties(vps_libpq_client_session_integration PROPERTIES
    SKIP_RETURN_CODE 77
    LABELS "session-integration"
  )

  if(TARGET vps_wincred_provider)
    add_executable(vps_wincred_provider_test
      tests/unit/vps_wincred_provider_test.c
    )
    target_compile_features(vps_wincred_provider_test PRIVATE c_std_11)
    set_target_properties(vps_wincred_provider_test PROPERTIES
      C_EXTENSIONS OFF
    )
    target_link_libraries(vps_wincred_provider_test PRIVATE
      vps_wincred_provider
    )
    vps_apply_strict_warnings(vps_wincred_provider_test)
    vps_apply_asan(vps_wincred_provider_test)
    add_test(NAME vps_wincred_provider COMMAND vps_wincred_provider_test)
  endif()

  if(TARGET virtualpostgresql AND
     EXISTS "${VPS_SQLITE_AMALGAMATION_DIR}/sqlite3.c")
    add_library(vps_sqlite_host STATIC
      "${VPS_SQLITE_AMALGAMATION_DIR}/sqlite3.c"
    )
    target_compile_features(vps_sqlite_host PRIVATE c_std_11)
    target_compile_definitions(vps_sqlite_host PRIVATE
      SQLITE_ENABLE_LOAD_EXTENSION
      SQLITE_THREADSAFE=1
    )
    target_include_directories(vps_sqlite_host PUBLIC
      "${VPS_SQLITE_AMALGAMATION_DIR}"
    )
    if(WIN32)
      target_link_libraries(vps_sqlite_host PUBLIC advapi32)
    else()
      target_link_libraries(vps_sqlite_host PUBLIC ${CMAKE_DL_LIBS})
    endif()

    add_executable(vps_extension_version_test
      tests/unit/vps_extension_version_test.c
      src/vps_extension.c
      src/vps_module.c
    )
    target_compile_features(vps_extension_version_test PRIVATE c_std_11)
    set_target_properties(vps_extension_version_test PROPERTIES C_EXTENSIONS OFF)
    target_include_directories(vps_extension_version_test PRIVATE
      "${CMAKE_CURRENT_SOURCE_DIR}/src"
      "${VPS_SQLITE_AMALGAMATION_DIR}"
    )
    target_link_libraries(vps_extension_version_test PRIVATE vps_sqlite_host)
    vps_apply_strict_warnings(vps_extension_version_test)
    add_test(NAME vps_extension_minimum_host
      COMMAND vps_extension_version_test)

    add_executable(vps_extension_host_test
      tests/host/vps_extension_host_test.c
    )
    target_compile_features(vps_extension_host_test PRIVATE c_std_11)
    set_target_properties(vps_extension_host_test PROPERTIES C_EXTENSIONS OFF)
    target_compile_definitions(vps_extension_host_test PRIVATE
      SQLITE_ENABLE_LOAD_EXTENSION
      SQLITE_THREADSAFE=1
    )
    target_include_directories(vps_extension_host_test PRIVATE
      "${VPS_SQLITE_AMALGAMATION_DIR}"
    )
    target_link_libraries(vps_extension_host_test PRIVATE vps_sqlite_host)
    vps_apply_strict_warnings(vps_extension_host_test)
    add_test(NAME vps_extension_host
      COMMAND vps_extension_host_test $<TARGET_FILE:virtualpostgresql>)
  endif()
endif()
