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
