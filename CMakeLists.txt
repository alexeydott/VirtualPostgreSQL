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

add_compile_definitions($<$<CONFIG:Debug>:VPS_DEBUG=1>)

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
option(VPS_BUILD_FUZZ "Build clang libFuzzer targets" OFF)

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

add_library(vps_client STATIC
  src/vps_client.c
)
target_compile_features(vps_client PUBLIC c_std_11)
set_target_properties(vps_client PROPERTIES
  C_EXTENSIONS OFF
  POSITION_INDEPENDENT_CODE ON
)
target_include_directories(vps_client PUBLIC
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>"
)
target_link_libraries(vps_client PUBLIC
  vps_error
  vps_logging
  vps_memory
)
vps_apply_strict_warnings(vps_client)
vps_apply_asan(vps_client)

add_library(vps_connection_pool STATIC
  src/vps_connection_pool.c
)
target_compile_features(vps_connection_pool PUBLIC c_std_11)
set_target_properties(vps_connection_pool PROPERTIES
  C_EXTENSIONS OFF
  POSITION_INDEPENDENT_CODE ON
)
target_include_directories(vps_connection_pool PUBLIC
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>"
)
target_link_libraries(vps_connection_pool PUBLIC
  vps_deadline
  vps_memory
  vps_platform
)
vps_apply_strict_warnings(vps_connection_pool)
vps_apply_asan(vps_connection_pool)

add_library(vps_retry STATIC
  src/vps_retry.c
)
target_compile_features(vps_retry PUBLIC c_std_11)
set_target_properties(vps_retry PROPERTIES C_EXTENSIONS OFF POSITION_INDEPENDENT_CODE ON)
target_include_directories(vps_retry PUBLIC
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>"
)
target_link_libraries(vps_retry PUBLIC vps_error vps_logging)
vps_apply_strict_warnings(vps_retry)
vps_apply_asan(vps_retry)

add_library(vps_metadata STATIC
  src/vps_metadata.c
  src/vps_metadata_key.c
  src/vps_metadata_policy.c
)
target_compile_features(vps_metadata PUBLIC c_std_11)
set_target_properties(vps_metadata PROPERTIES
  C_EXTENSIONS OFF
  POSITION_INDEPENDENT_CODE ON
)
target_include_directories(vps_metadata PUBLIC
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>"
)
target_link_libraries(vps_metadata PUBLIC vps_logging vps_memory)
vps_apply_strict_warnings(vps_metadata)
vps_apply_asan(vps_metadata)

add_library(vps_type_registry STATIC
  src/vps_type_registry.c
)
target_compile_features(vps_type_registry PUBLIC c_std_11)
set_target_properties(vps_type_registry PROPERTIES
  C_EXTENSIONS OFF
  POSITION_INDEPENDENT_CODE ON
)
target_include_directories(vps_type_registry PUBLIC
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>"
)
target_link_libraries(vps_type_registry PUBLIC vps_metadata)
vps_apply_strict_warnings(vps_type_registry)
vps_apply_asan(vps_type_registry)

add_library(vps_schema_fingerprint STATIC
  src/vps_schema_fingerprint.c
)
target_compile_features(vps_schema_fingerprint PUBLIC c_std_11)
set_target_properties(vps_schema_fingerprint PROPERTIES
  C_EXTENSIONS OFF
  POSITION_INDEPENDENT_CODE ON
)
target_include_directories(vps_schema_fingerprint PUBLIC
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>"
)
target_link_libraries(vps_schema_fingerprint PUBLIC
  vps_metadata
  vps_type_registry
)
vps_apply_strict_warnings(vps_schema_fingerprint)
vps_apply_asan(vps_schema_fingerprint)

add_library(vps_query_source STATIC
  src/vps_query_source.c
)
target_compile_features(vps_query_source PUBLIC c_std_11)
set_target_properties(vps_query_source PROPERTIES
  C_EXTENSIONS OFF
  POSITION_INDEPENDENT_CODE ON
)
target_include_directories(vps_query_source PUBLIC
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>"
)
target_link_libraries(vps_query_source PUBLIC vps_logging)
vps_apply_strict_warnings(vps_query_source)
vps_apply_asan(vps_query_source)

if(VPS_BUILD_FUZZ)
  if(NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "[fuzz] VPS_BUILD_FUZZ requires Clang/clang-cl")
  endif()
  add_executable(vps_query_source_fuzz
    tests/fuzz/vps_query_source_fuzz.c
  )
  target_compile_features(vps_query_source_fuzz PRIVATE c_std_11)
  set_target_properties(vps_query_source_fuzz PROPERTIES C_EXTENSIONS OFF)
  target_link_libraries(vps_query_source_fuzz PRIVATE vps_query_source)
  if(CMAKE_C_SIMULATE_ID STREQUAL "MSVC")
    execute_process(
      COMMAND "${CMAKE_C_COMPILER}" --print-resource-dir
      OUTPUT_VARIABLE VPS_CLANG_RESOURCE_DIR
      OUTPUT_STRIP_TRAILING_WHITESPACE
      RESULT_VARIABLE VPS_CLANG_RESOURCE_RESULT
    )
    if(NOT VPS_CLANG_RESOURCE_RESULT EQUAL 0)
      message(FATAL_ERROR "[fuzz] unable to locate the clang runtime directory")
    endif()
    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
      set(VPS_LIBFUZZER_RUNTIME
        "${VPS_CLANG_RESOURCE_DIR}/lib/windows/clang_rt.fuzzer-x86_64.lib")
    else()
      set(VPS_LIBFUZZER_RUNTIME
        "${VPS_CLANG_RESOURCE_DIR}/lib/windows/clang_rt.fuzzer-i386.lib")
    endif()
    if(NOT EXISTS "${VPS_LIBFUZZER_RUNTIME}")
      message(FATAL_ERROR "[fuzz] clang libFuzzer runtime is missing")
    endif()
    target_compile_options(vps_query_source_fuzz PRIVATE
      -fsanitize=fuzzer-no-link)
    target_link_libraries(vps_query_source_fuzz PRIVATE
      "${VPS_LIBFUZZER_RUNTIME}")
  else()
    target_compile_options(vps_query_source_fuzz PRIVATE -fsanitize=fuzzer)
    target_link_options(vps_query_source_fuzz PRIVATE -fsanitize=fuzzer)
  endif()
  vps_apply_strict_warnings(vps_query_source_fuzz)
endif()

add_library(vps_query_profile STATIC
  src/vps_query_profile.c
)
target_compile_features(vps_query_profile PUBLIC c_std_11)
set_target_properties(vps_query_profile PROPERTIES
  C_EXTENSIONS OFF
  POSITION_INDEPENDENT_CODE ON
)
target_include_directories(vps_query_profile PUBLIC
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>"
)
target_link_libraries(vps_query_profile PUBLIC
  vps_memory
  vps_platform
  vps_query_source
)
vps_apply_strict_warnings(vps_query_profile)
vps_apply_asan(vps_query_profile)

add_library(vps_query_validation STATIC
  src/vps_query_validation.c
)
target_compile_features(vps_query_validation PUBLIC c_std_11)
set_target_properties(vps_query_validation PROPERTIES
  C_EXTENSIONS OFF
  POSITION_INDEPENDENT_CODE ON
)
target_include_directories(vps_query_validation PUBLIC
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>"
)
target_link_libraries(vps_query_validation PUBLIC
  vps_client
  vps_query_source
)
vps_apply_strict_warnings(vps_query_validation)
vps_apply_asan(vps_query_validation)

add_library(vps_query_metadata STATIC
  src/vps_query_indexes.c
  src/vps_query_metadata.c
)
target_compile_features(vps_query_metadata PUBLIC c_std_11)
set_target_properties(vps_query_metadata PROPERTIES
  C_EXTENSIONS OFF
  POSITION_INDEPENDENT_CODE ON
)
target_include_directories(vps_query_metadata PUBLIC
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>"
)
target_link_libraries(vps_query_metadata PUBLIC
  vps_query_validation
  vps_logging
  vps_memory
)
vps_apply_strict_warnings(vps_query_metadata)
vps_apply_asan(vps_query_metadata)

add_library(vps_query_boundary STATIC
  src/vps_query_boundary.c
)
target_compile_features(vps_query_boundary PUBLIC c_std_11)
set_target_properties(vps_query_boundary PROPERTIES
  C_EXTENSIONS OFF
  POSITION_INDEPENDENT_CODE ON
)
target_include_directories(vps_query_boundary PUBLIC
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>"
)
target_link_libraries(vps_query_boundary PUBLIC vps_error vps_logging)
vps_apply_strict_warnings(vps_query_boundary)
vps_apply_asan(vps_query_boundary)

add_library(vps_query_fingerprint STATIC
  src/vps_query_fingerprint.c
)
target_compile_features(vps_query_fingerprint PUBLIC c_std_11)
set_target_properties(vps_query_fingerprint PROPERTIES
  C_EXTENSIONS OFF
  POSITION_INDEPENDENT_CODE ON
)
target_include_directories(vps_query_fingerprint PUBLIC
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>"
)
target_link_libraries(vps_query_fingerprint PUBLIC vps_query_metadata)
vps_apply_strict_warnings(vps_query_fingerprint)
vps_apply_asan(vps_query_fingerprint)

add_library(vps_planner STATIC
  src/vps_planner.c
)
target_compile_features(vps_planner PUBLIC c_std_11)
set_target_properties(vps_planner PROPERTIES
  C_EXTENSIONS OFF
  POSITION_INDEPENDENT_CODE ON
)
target_include_directories(vps_planner PUBLIC
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>"
)
target_link_libraries(vps_planner PUBLIC vps_logging vps_memory)
vps_apply_strict_warnings(vps_planner)
vps_apply_asan(vps_planner)

if(VPS_BUILD_FUZZ)
  add_executable(vps_planner_fuzz tests/fuzz/vps_planner_fuzz.c)
  target_compile_features(vps_planner_fuzz PRIVATE c_std_11)
  set_target_properties(vps_planner_fuzz PROPERTIES C_EXTENSIONS OFF)
  target_link_libraries(vps_planner_fuzz PRIVATE vps_planner)
  if(CMAKE_C_SIMULATE_ID STREQUAL "MSVC")
    target_compile_options(vps_planner_fuzz PRIVATE -fsanitize=fuzzer-no-link)
    target_link_libraries(vps_planner_fuzz PRIVATE
      "${VPS_LIBFUZZER_RUNTIME}")
  else()
    target_compile_options(vps_planner_fuzz PRIVATE -fsanitize=fuzzer)
    target_link_options(vps_planner_fuzz PRIVATE -fsanitize=fuzzer)
  endif()
  vps_apply_strict_warnings(vps_planner_fuzz)
endif()

add_library(vps_type_codec STATIC
  src/vps_type_codec.c
)
target_compile_features(vps_type_codec PUBLIC c_std_11)
set_target_properties(vps_type_codec PROPERTIES
  C_EXTENSIONS OFF
  POSITION_INDEPENDENT_CODE ON
)
target_include_directories(vps_type_codec PUBLIC
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>"
)
target_link_libraries(vps_type_codec PUBLIC vps_client vps_type_registry)
vps_apply_strict_warnings(vps_type_codec)
vps_apply_asan(vps_type_codec)

add_library(vps_row_identity STATIC
  src/vps_row_identity.c
)
target_compile_features(vps_row_identity PUBLIC c_std_11)
set_target_properties(vps_row_identity PROPERTIES
  C_EXTENSIONS OFF
  POSITION_INDEPENDENT_CODE ON
)
target_include_directories(vps_row_identity PUBLIC
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>"
)
target_link_libraries(vps_row_identity PUBLIC vps_client vps_memory)
vps_apply_strict_warnings(vps_row_identity)
vps_apply_asan(vps_row_identity)

add_library(vps_libpq_client_metadata STATIC
  src/vps_libpq_client_metadata.c
)
target_compile_features(vps_libpq_client_metadata PUBLIC c_std_11)
set_target_properties(vps_libpq_client_metadata PROPERTIES
  C_EXTENSIONS OFF
  POSITION_INDEPENDENT_CODE ON
)
target_include_directories(vps_libpq_client_metadata PUBLIC
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>"
)
target_link_libraries(vps_libpq_client_metadata PUBLIC vps_client vps_metadata)
vps_apply_strict_warnings(vps_libpq_client_metadata)
vps_apply_asan(vps_libpq_client_metadata)

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

  add_library(vps_libpq_client STATIC
    src/vps_libpq_client.c
    src/vps_libpq_client_health.c
    src/vps_libpq_client_statement.c
  )
  target_compile_features(vps_libpq_client PUBLIC c_std_11)
  set_target_properties(vps_libpq_client PROPERTIES
    C_EXTENSIONS OFF
    POSITION_INDEPENDENT_CODE ON
  )
  target_include_directories(vps_libpq_client
    PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>"
    PRIVATE "${VPS_LIBPQ_INCLUDE_DIR}"
  )
  target_link_libraries(vps_libpq_client
    PUBLIC
      vps_client
      vps_deadline
      vps_identity
      vps_session
      vps_libpq_client_tls
    PRIVATE
      "${VPS_LIBPQ_LIBRARY_DIR}/libpq.lib"
      "${VPS_LIBPQ_LIBRARY_DIR}/pgcommon.lib"
      "${VPS_LIBPQ_LIBRARY_DIR}/pgport.lib"
      "${VPS_OPENSSL_LIBRARY_DIR}/libssl.lib"
      "${VPS_OPENSSL_LIBRARY_DIR}/libcrypto.lib"
      "${VPS_ZLIB_LIBRARY_DIR}/zlib.lib"
      ws2_32 secur32 crypt32 advapi32 user32 shell32
  )
  vps_apply_strict_warnings(vps_libpq_client)
  vps_apply_asan(vps_libpq_client)
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
  target_link_libraries(virtualpostgresql PRIVATE
    vps_abi
    vps_arguments
    vps_connection_string
    vps_connection_pool
    vps_identity
    vps_libpq_client
    vps_libpq_client_conninfo
    vps_planner
    vps_query_validation
    vps_row_identity
    vps_type_codec
  )
  if(WIN32 AND VPS_ENABLE_WINCRED)
    target_link_libraries(virtualpostgresql PRIVATE vps_wincred_provider)
  endif()
  vps_apply_strict_warnings(virtualpostgresql)
  vps_apply_asan(virtualpostgresql)
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

  add_executable(vps_type_codec_test tests/unit/vps_type_codec_test.c)
  target_compile_features(vps_type_codec_test PRIVATE c_std_11)
  set_target_properties(vps_type_codec_test PROPERTIES C_EXTENSIONS OFF)
  target_link_libraries(vps_type_codec_test PRIVATE vps_type_codec)
  vps_apply_strict_warnings(vps_type_codec_test)
  vps_apply_asan(vps_type_codec_test)
  add_test(NAME vps_type_codec COMMAND vps_type_codec_test)

  add_executable(vps_row_identity_test tests/unit/vps_row_identity_test.c)
  target_compile_features(vps_row_identity_test PRIVATE c_std_11)
  set_target_properties(vps_row_identity_test PROPERTIES C_EXTENSIONS OFF)
  target_link_libraries(vps_row_identity_test PRIVATE vps_row_identity)
  vps_apply_strict_warnings(vps_row_identity_test)
  vps_apply_asan(vps_row_identity_test)
  add_test(NAME vps_row_identity COMMAND vps_row_identity_test)

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

  add_executable(vps_client_test
    tests/unit/vps_client_test.c
  )
  target_compile_features(vps_client_test PRIVATE c_std_11)
  set_target_properties(vps_client_test PROPERTIES C_EXTENSIONS OFF)
  target_link_libraries(vps_client_test PRIVATE vps_client)
  vps_apply_strict_warnings(vps_client_test)
  vps_apply_asan(vps_client_test)
  add_test(NAME vps_client COMMAND vps_client_test)

  add_executable(vps_connection_pool_test
    tests/unit/vps_connection_pool_test.c
  )
  target_compile_features(vps_connection_pool_test PRIVATE c_std_11)
  set_target_properties(vps_connection_pool_test PROPERTIES C_EXTENSIONS OFF)
  target_link_libraries(vps_connection_pool_test PRIVATE vps_connection_pool)
  vps_apply_strict_warnings(vps_connection_pool_test)
  vps_apply_asan(vps_connection_pool_test)
  add_test(NAME vps_connection_pool COMMAND vps_connection_pool_test)

  add_executable(vps_retry_test tests/unit/vps_retry_test.c)
  target_compile_features(vps_retry_test PRIVATE c_std_11)
  set_target_properties(vps_retry_test PROPERTIES C_EXTENSIONS OFF)
  target_link_libraries(vps_retry_test PRIVATE vps_retry)
  vps_apply_strict_warnings(vps_retry_test)
  vps_apply_asan(vps_retry_test)
  add_test(NAME vps_retry COMMAND vps_retry_test)

  add_executable(vps_metadata_test tests/unit/vps_metadata_test.c)
  target_compile_features(vps_metadata_test PRIVATE c_std_11)
  set_target_properties(vps_metadata_test PROPERTIES C_EXTENSIONS OFF)
  target_link_libraries(vps_metadata_test PRIVATE
    vps_libpq_client_metadata
    vps_metadata
  )
  vps_apply_strict_warnings(vps_metadata_test)
  vps_apply_asan(vps_metadata_test)
  add_test(NAME vps_metadata COMMAND vps_metadata_test)

  add_executable(vps_type_registry_test
    tests/unit/vps_type_registry_test.c
  )
  target_compile_features(vps_type_registry_test PRIVATE c_std_11)
  set_target_properties(vps_type_registry_test PROPERTIES C_EXTENSIONS OFF)
  target_link_libraries(vps_type_registry_test PRIVATE vps_type_registry)
  vps_apply_strict_warnings(vps_type_registry_test)
  vps_apply_asan(vps_type_registry_test)
  add_test(NAME vps_type_registry COMMAND vps_type_registry_test)

  add_executable(vps_schema_fingerprint_test
    tests/unit/vps_schema_fingerprint_test.c
  )
  target_compile_features(vps_schema_fingerprint_test PRIVATE c_std_11)
  set_target_properties(vps_schema_fingerprint_test PROPERTIES
    C_EXTENSIONS OFF
  )
  target_link_libraries(vps_schema_fingerprint_test PRIVATE
    vps_schema_fingerprint
  )
  vps_apply_strict_warnings(vps_schema_fingerprint_test)
  vps_apply_asan(vps_schema_fingerprint_test)
  add_test(NAME vps_schema_fingerprint COMMAND vps_schema_fingerprint_test)

  add_executable(vps_query_source_test
    tests/unit/vps_query_source_test.c
  )
  target_compile_features(vps_query_source_test PRIVATE c_std_11)
  set_target_properties(vps_query_source_test PROPERTIES C_EXTENSIONS OFF)
  target_link_libraries(vps_query_source_test PRIVATE vps_query_source)
  vps_apply_strict_warnings(vps_query_source_test)
  vps_apply_asan(vps_query_source_test)
  add_test(NAME vps_query_source COMMAND vps_query_source_test)

  add_executable(vps_query_profile_test
    tests/unit/vps_query_profile_test.c
  )
  target_compile_features(vps_query_profile_test PRIVATE c_std_11)
  set_target_properties(vps_query_profile_test PROPERTIES C_EXTENSIONS OFF)
  target_link_libraries(vps_query_profile_test PRIVATE vps_query_profile)
  vps_apply_strict_warnings(vps_query_profile_test)
  vps_apply_asan(vps_query_profile_test)
  add_test(NAME vps_query_profile COMMAND vps_query_profile_test)

  add_executable(vps_query_validation_test
    tests/unit/vps_query_validation_test.c
  )
  target_compile_features(vps_query_validation_test PRIVATE c_std_11)
  set_target_properties(vps_query_validation_test PROPERTIES C_EXTENSIONS OFF)
  target_link_libraries(vps_query_validation_test PRIVATE
    vps_query_validation
  )
  vps_apply_strict_warnings(vps_query_validation_test)
  vps_apply_asan(vps_query_validation_test)
  add_test(NAME vps_query_validation COMMAND vps_query_validation_test)

  add_executable(vps_query_metadata_test
    tests/unit/vps_query_metadata_test.c
  )
  target_compile_features(vps_query_metadata_test PRIVATE c_std_11)
  set_target_properties(vps_query_metadata_test PROPERTIES C_EXTENSIONS OFF)
  target_link_libraries(vps_query_metadata_test PRIVATE vps_query_metadata)
  vps_apply_strict_warnings(vps_query_metadata_test)
  vps_apply_asan(vps_query_metadata_test)
  add_test(NAME vps_query_metadata COMMAND vps_query_metadata_test)

  add_executable(vps_query_boundary_test
    tests/unit/vps_query_boundary_test.c
  )
  target_compile_features(vps_query_boundary_test PRIVATE c_std_11)
  set_target_properties(vps_query_boundary_test PROPERTIES C_EXTENSIONS OFF)
  target_link_libraries(vps_query_boundary_test PRIVATE vps_query_boundary)
  vps_apply_strict_warnings(vps_query_boundary_test)
  vps_apply_asan(vps_query_boundary_test)
  add_test(NAME vps_query_boundary COMMAND vps_query_boundary_test)

  add_executable(vps_query_fingerprint_test
    tests/unit/vps_query_fingerprint_test.c
  )
  target_compile_features(vps_query_fingerprint_test PRIVATE c_std_11)
  set_target_properties(vps_query_fingerprint_test PROPERTIES C_EXTENSIONS OFF)
  target_link_libraries(vps_query_fingerprint_test PRIVATE
    vps_query_fingerprint
  )
  vps_apply_strict_warnings(vps_query_fingerprint_test)
  vps_apply_asan(vps_query_fingerprint_test)
  add_test(NAME vps_query_fingerprint COMMAND vps_query_fingerprint_test)

  add_executable(vps_planner_test tests/unit/vps_planner_test.c)
  target_compile_features(vps_planner_test PRIVATE c_std_11)
  set_target_properties(vps_planner_test PROPERTIES C_EXTENSIONS OFF)
  target_link_libraries(vps_planner_test PRIVATE vps_planner)
  vps_apply_strict_warnings(vps_planner_test)
  vps_apply_asan(vps_planner_test)
  add_test(NAME vps_planner COMMAND vps_planner_test)

  add_executable(vps_planner_fuzz_smoke_test
    tests/unit/vps_planner_fuzz_smoke_test.c)
  target_compile_features(vps_planner_fuzz_smoke_test PRIVATE c_std_11)
  set_target_properties(vps_planner_fuzz_smoke_test PROPERTIES C_EXTENSIONS OFF)
  target_link_libraries(vps_planner_fuzz_smoke_test PRIVATE vps_planner)
  vps_apply_strict_warnings(vps_planner_fuzz_smoke_test)
  vps_apply_asan(vps_planner_fuzz_smoke_test)
  add_test(NAME vps_planner_fuzz_smoke COMMAND vps_planner_fuzz_smoke_test)

  add_executable(vps_libpq_client_test
    tests/unit/vps_libpq_client_test.c
  )
  target_compile_features(vps_libpq_client_test PRIVATE c_std_11)
  set_target_properties(vps_libpq_client_test PROPERTIES C_EXTENSIONS OFF)
  target_link_libraries(vps_libpq_client_test PRIVATE vps_libpq_client)
  vps_apply_strict_warnings(vps_libpq_client_test)
  vps_apply_asan(vps_libpq_client_test)
  add_test(NAME vps_libpq_client COMMAND vps_libpq_client_test)

  add_executable(vps_client_stream_performance_test
    tests/performance/vps_client_stream_performance_test.c
  )
  target_compile_features(vps_client_stream_performance_test PRIVATE c_std_11)
  set_target_properties(vps_client_stream_performance_test PROPERTIES C_EXTENSIONS OFF)
  target_link_libraries(vps_client_stream_performance_test PRIVATE vps_client)
  vps_apply_strict_warnings(vps_client_stream_performance_test)
  vps_apply_asan(vps_client_stream_performance_test)
  add_test(NAME vps_client_stream_performance COMMAND vps_client_stream_performance_test)
  set_tests_properties(vps_client_stream_performance PROPERTIES LABELS "performance")

  add_executable(vps_libpq_client_connect_integration_test
    tests/integration/vps_libpq_client_connect_integration_test.c
  )
  target_compile_features(vps_libpq_client_connect_integration_test
    PRIVATE c_std_11)
  set_target_properties(vps_libpq_client_connect_integration_test PROPERTIES
    C_EXTENSIONS OFF
  )
  if(MSVC)
    target_compile_definitions(
      vps_libpq_client_connect_integration_test PRIVATE
      _CRT_SECURE_NO_WARNINGS
    )
  endif()
  target_link_libraries(vps_libpq_client_connect_integration_test PRIVATE
    vps_libpq_client
    vps_libpq_client_metadata
    vps_connection_pool
    vps_query_validation
    vps_schema_fingerprint
  )
  vps_apply_strict_warnings(vps_libpq_client_connect_integration_test)
  vps_apply_asan(vps_libpq_client_connect_integration_test)
  add_test(NAME vps_libpq_client_connect_integration
    COMMAND vps_libpq_client_connect_integration_test)
  set_tests_properties(vps_libpq_client_connect_integration PROPERTIES
    SKIP_RETURN_CODE 77
    LABELS "async-connect-integration"
  )

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
      "${CMAKE_CURRENT_SOURCE_DIR}/include"
      "${CMAKE_CURRENT_SOURCE_DIR}/src"
      "${VPS_SQLITE_AMALGAMATION_DIR}"
    )
    target_link_libraries(vps_extension_version_test PRIVATE
      vps_sqlite_host
      vps_arguments
      vps_connection_string
      vps_connection_pool
      vps_identity
      vps_libpq_client
      vps_libpq_client_conninfo
      vps_planner
      vps_query_validation
      vps_row_identity
      vps_type_codec
    )
    if(WIN32 AND VPS_ENABLE_WINCRED)
      target_link_libraries(vps_extension_version_test PRIVATE
        vps_wincred_provider)
    endif()
    vps_apply_strict_warnings(vps_extension_version_test)
    vps_apply_asan(vps_extension_version_test)
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
