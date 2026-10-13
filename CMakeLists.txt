# Copyright (c) 2018-2026 Giuseppe Marino
# SPDX-License-Identifier: BSD-3-Clause

cmake_minimum_required(VERSION 3.20)

file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/dist.info" _tdlua_dist_version_line
     REGEX "^version[ \t]*=")
string(REGEX MATCH "\"([0-9]+\\.[0-9]+(\\.[0-9]+)?)\""
       _tdlua_dist_version_match "${_tdlua_dist_version_line}")
if(NOT _tdlua_dist_version_match)
  message(FATAL_ERROR "Unable to read TDLua version from dist.info")
endif()
set(_tdlua_base_version "${CMAKE_MATCH_1}")
project(tdlua VERSION "${_tdlua_base_version}" LANGUAGES C CXX)

set(CMAKE_POSITION_INDEPENDENT_CODE ON)
# Keep the compilation database enabled for clangd and static analysis. This
# is a project setting rather than a user-selectable backend option.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(TDLUA_BASE_VERSION "${PROJECT_VERSION}")
option(TDLUA_BUILD_TESTS "Build TDLua regression tests" ON)
option(TDLUA_BUNDLED_TDLIB "Build the TDLib copy in the td subdirectory" ON)
set(TDLUA_TDLIB_CODEGEN_BUILD_DIR "" CACHE PATH
    "Existing bundled TDLua build containing TDLib native codec tools (required for native builds with TDLUA_BUNDLED_TDLIB=OFF)")
option(TDLUA_NATIVE_BACKEND "Build TDLua against TDLib's native C++ interface" OFF)
option(TDLUA_ENABLE_SANITIZERS
       "Enable AddressSanitizer and UndefinedBehaviorSanitizer for TDLua targets"
       OFF)

function(tdlua_enable_sanitizers target)
  if(TDLUA_ENABLE_SANITIZERS)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
      message(FATAL_ERROR
              "TDLUA_ENABLE_SANITIZERS requires a GCC- or Clang-compatible compiler")
    endif()
    target_compile_options(${target} PRIVATE
        -fsanitize=address,undefined
        -fno-omit-frame-pointer
        -fno-sanitize=vptr)
    target_link_options(${target} PRIVATE -fsanitize=address,undefined)
  endif()
endfunction()

if(TDLUA_ENABLE_SANITIZERS AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
  execute_process(
      COMMAND "${CMAKE_CXX_COMPILER}" -print-file-name=libasan.so
      OUTPUT_VARIABLE _tdlua_asan_runtime
      OUTPUT_STRIP_TRAILING_WHITESPACE
      RESULT_VARIABLE _tdlua_asan_result)
  execute_process(
      COMMAND "${CMAKE_CXX_COMPILER}" -print-file-name=libubsan.so
      OUTPUT_VARIABLE _tdlua_ubsan_runtime
      OUTPUT_STRIP_TRAILING_WHITESPACE
      RESULT_VARIABLE _tdlua_ubsan_result)
  if(NOT _tdlua_asan_result EQUAL 0
     OR NOT _tdlua_ubsan_result EQUAL 0
     OR NOT IS_ABSOLUTE "${_tdlua_asan_runtime}"
     OR NOT IS_ABSOLUTE "${_tdlua_ubsan_runtime}")
    message(FATAL_ERROR
            "Unable to locate ASan/UBSan runtimes for sanitized Lua tests")
  endif()
  set(_tdlua_sanitizer_test_environment
      "LD_PRELOAD=${_tdlua_asan_runtime}:${_tdlua_ubsan_runtime}")
endif()

function(tdlua_enable_sanitizer_test_runtime)
  if(_tdlua_sanitizer_test_environment)
    set_tests_properties(${ARGV}
        PROPERTIES ENVIRONMENT "${_tdlua_sanitizer_test_environment}")
  endif()
endfunction()

set(_tdlua_backend_was_defined FALSE)
if(DEFINED TDLUA_BACKEND)
  set(_tdlua_backend_was_defined TRUE)
endif()
set(TDLUA_BACKEND "json" CACHE STRING
    "TDLua backend to build (json or native)")
set_property(CACHE TDLUA_BACKEND PROPERTY STRINGS json native)
set(TDLUA_NATIVE_SHARDS "16" CACHE STRING
    "Number of generated native codec translation units")
option(TDLUA_JSON_STATIC
       "Link the JSON backend against the static TDLib JSON library" OFF)
option(TDLUA_TD_STATIC "Legacy alias for TDLUA_JSON_STATIC" OFF)

# TDLUA_NATIVE_BACKEND is retained for existing build scripts. The explicit
# backend option is the canonical interface for new builds.
if(TDLUA_NATIVE_BACKEND)
  message(DEPRECATION
          "TDLUA_NATIVE_BACKEND is deprecated; use -DTDLUA_BACKEND=native")
  if(_tdlua_backend_was_defined AND NOT TDLUA_BACKEND STREQUAL "native")
    message(WARNING
            "TDLUA_NATIVE_BACKEND=ON overrides TDLUA_BACKEND=${TDLUA_BACKEND}; "
            "use TDLUA_BACKEND=native explicitly")
  endif()
  set(_tdlua_backend "native")
else()
  set(_tdlua_backend "${TDLUA_BACKEND}")
endif()
# Keep the cache and generated package metadata aligned with the effective
# backend, including when the deprecated native alias selected it.
set(TDLUA_BACKEND "${_tdlua_backend}" CACHE STRING
    "TDLua backend to build (json or native)" FORCE)
if(_tdlua_backend STREQUAL "native")
  set(_tdlua_native_backend ON)
elseif(_tdlua_backend STREQUAL "json")
  set(_tdlua_native_backend OFF)
else()
  message(FATAL_ERROR
          "TDLUA_BACKEND must be either 'json' or 'native'")
endif()

if(NOT TDLUA_NATIVE_SHARDS MATCHES "^[1-9][0-9]*$")
  message(FATAL_ERROR "TDLUA_NATIVE_SHARDS must be a positive integer")
endif()

if(TDLUA_TD_STATIC)
  if(NOT TDLUA_JSON_STATIC)
    message(STATUS
            "TDLUA_TD_STATIC is deprecated; enabling TDLUA_JSON_STATIC")
  endif()
  set(_tdlua_legacy_json_static ON)
endif()

set(_tdlua_json_static "${TDLUA_JSON_STATIC}")
if(DEFINED _tdlua_legacy_json_static)
  set(_tdlua_json_static ON)
endif()
if(_tdlua_backend STREQUAL "native" AND TDLUA_JSON_STATIC)
  message(WARNING
          "TDLUA_JSON_STATIC is ignored for the native backend; native is always static")
  set(_tdlua_json_static OFF)
endif()
set(TDLUA_JSON_STATIC "${_tdlua_json_static}")

set(TDLUA_TDLIB_VERSION "")
set(TDLUA_LUA_VERSION "" CACHE STRING "Lua version used for the module installation path")
set(TDLUA_LUA_MODULE_DIR "" CACHE STRING "Installation directory for the tdlua Lua module")
include(GNUInstallDirs)

# TDLib has its own CTest and benchmark targets. TDLua controls its tests
# below, so keep the vendored dependency from adding its full test tree.
set(BUILD_TESTING OFF CACHE BOOL "Disable TDLib tests when building TDLua" FORCE)

# Test builds expose diagnostic-only Lua methods from the module. Refuse to
# install that module so a default test build cannot become a release package.
if(TDLUA_BUILD_TESTS)
  install(CODE [[
    message(FATAL_ERROR
      "This TDLua build includes test instrumentation and cannot be installed. Reconfigure with -DTDLUA_BUILD_TESTS=OFF.")
  ]])
endif()

set(TDLUA_LUA_IMPLEMENTATION "auto" CACHE STRING "Lua implementation: auto, lua or luajit")
set_property(CACHE TDLUA_LUA_IMPLEMENTATION PROPERTY STRINGS auto lua luajit)
if(NOT TDLUA_LUA_IMPLEMENTATION MATCHES "^(auto|lua|luajit)$")
    message(FATAL_ERROR "TDLUA_LUA_IMPLEMENTATION must be auto, lua or luajit")
endif()
if(TDLUA_LUA_IMPLEMENTATION STREQUAL "luajit")
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(LUAJIT QUIET luajit)
    endif()
    find_path(LUA_INCLUDE_DIR NAMES luajit.h HINTS ${LUAJIT_INCLUDE_DIRS}
        PATH_SUFFIXES luajit-2.1)
    find_library(LUA_LIBRARY NAMES luajit-5.1 luajit HINTS ${LUAJIT_LIBRARY_DIRS})
    if(NOT LUA_INCLUDE_DIR OR NOT LUA_LIBRARY)
        message(FATAL_ERROR "LuaJIT 2.1 development headers and library were not found")
    endif()
    set(TDLUA_LUA_VERSION "5.1" CACHE STRING "LuaJIT installation ABI" FORCE)
    if(NOT LUA_EXECUTABLE)
        find_program(LUA_EXECUTABLE NAMES luajit)
    endif()
endif()

if(TDLUA_LUA_VERSION AND LUA_INCLUDE_DIR AND LUA_LIBRARY)
  # Older CMake FindLua modules do not recognize Lua 5.5 headers. When the
  # caller supplies an explicit Lua version and paths, use those paths
  # directly and add the libraries needed by a static Lua build.
  set(LUA_VERSION_STRING "${TDLUA_LUA_VERSION}")
  set(LUA_INCLUDE_DIRS "${LUA_INCLUDE_DIR}")
  set(LUA_LIBRARIES "${LUA_LIBRARY}")
  if(UNIX AND NOT APPLE AND NOT BEOS)
    find_library(LUA_MATH_LIBRARY m)
    if(LUA_MATH_LIBRARY)
      list(APPEND LUA_LIBRARIES "${LUA_MATH_LIBRARY}")
    endif()
    if(LUA_LIBRARY MATCHES "\\.a$" AND CMAKE_DL_LIBS)
      list(APPEND LUA_LIBRARIES "${CMAKE_DL_LIBS}")
    endif()
  endif()
elseif(TDLUA_LUA_VERSION)
  find_package(Lua ${TDLUA_LUA_VERSION} EXACT REQUIRED)
else()
  find_package(Lua REQUIRED)
endif()
if(NOT LUA_INCLUDE_DIRS)
  set(LUA_INCLUDE_DIRS "${LUA_INCLUDE_DIR}")
endif()

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/LuaRuntime.cmake")

if(TDLUA_BUNDLED_TDLIB)
  # TDLib defines both targets internally, but install only the library form
  # needed by the selected backend.
  if(_tdlua_backend STREQUAL "json" AND NOT _tdlua_json_static)
    set(TD_INSTALL_SHARED_LIBRARIES ON CACHE BOOL "" FORCE)
    set(TD_INSTALL_STATIC_LIBRARIES OFF CACHE BOOL "" FORCE)
  else()
    set(TD_INSTALL_SHARED_LIBRARIES OFF CACHE BOOL "" FORCE)
    set(TD_INSTALL_STATIC_LIBRARIES ON CACHE BOOL "" FORCE)
  endif()
  add_subdirectory(td)
  if(_tdlua_native_backend)
    set(_tdlua_tdjson_private_target tdjson_private)
  endif()
else()
  if(_tdlua_native_backend)
    file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/td/CMakeLists.txt"
         _tdlua_tdlib_project_line REGEX "^project\\(TDLib VERSION ")
    string(REGEX MATCH "VERSION ([0-9]+\\.[0-9]+\\.[0-9]+)"
           _tdlua_tdlib_project_match "${_tdlua_tdlib_project_line}")
    if(NOT _tdlua_tdlib_project_match)
      message(FATAL_ERROR
              "Native external TDLib requires the pinned td submodule source to validate the codec schema version")
    endif()
    set(_tdlua_tdlib_source_version "${CMAKE_MATCH_1}")
    find_package(Td "${_tdlua_tdlib_source_version}" EXACT CONFIG REQUIRED)
    if(NOT TARGET Td::TdStatic)
      message(FATAL_ERROR
              "The selected TDLib package does not export the required Td::TdStatic target")
    endif()

    find_program(_tdlua_git_executable git)
    if(NOT _tdlua_git_executable)
      message(FATAL_ERROR
              "Native external TDLib needs git to verify the td submodule matches this checkout's pinned schema source")
    endif()
    execute_process(
        COMMAND "${_tdlua_git_executable}" -C "${CMAKE_CURRENT_SOURCE_DIR}" ls-tree HEAD td
        OUTPUT_VARIABLE _tdlua_pinned_td_entry
        OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE _tdlua_gitlink_result)
    execute_process(
        COMMAND "${_tdlua_git_executable}" -C "${CMAKE_CURRENT_SOURCE_DIR}/td" rev-parse HEAD
        OUTPUT_VARIABLE _tdlua_td_source_commit
        OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE _tdlua_td_commit_result)
    string(REGEX MATCH "160000 commit ([0-9a-f]+)[ \t]+td$"
           _tdlua_gitlink_match "${_tdlua_pinned_td_entry}")
    set(_tdlua_pinned_td_commit "${CMAKE_MATCH_1}")
    if(NOT _tdlua_gitlink_result EQUAL 0 OR NOT _tdlua_td_commit_result EQUAL 0
       OR NOT _tdlua_pinned_td_commit
       OR NOT _tdlua_td_source_commit STREQUAL _tdlua_pinned_td_commit)
      message(FATAL_ERROR
              "The td submodule checkout does not match the commit pinned by this TDLua revision; refusing to generate a native codec from an unverified schema")
    endif()
    execute_process(
        COMMAND "${_tdlua_git_executable}" -C "${CMAKE_CURRENT_SOURCE_DIR}/td" status --porcelain
        OUTPUT_VARIABLE _tdlua_td_source_changes
        OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE _tdlua_td_status_result)
    if(NOT _tdlua_td_status_result EQUAL 0 OR _tdlua_td_source_changes)
      message(FATAL_ERROR
              "The td submodule has local changes; external native codec generation requires the clean pinned schema source")
    endif()

    if(NOT TDLUA_TDLIB_CODEGEN_BUILD_DIR)
      message(FATAL_ERROR
              "Native external TDLib requires -DTDLUA_TDLIB_CODEGEN_BUILD_DIR=<existing bundled build>; the TDLib install package does not export its native codec generator or schema compiler")
    endif()
    get_filename_component(_tdlua_codegen_build_dir
                           "${TDLUA_TDLIB_CODEGEN_BUILD_DIR}" REALPATH)
    if(NOT EXISTS "${_tdlua_codegen_build_dir}/CMakeCache.txt")
      message(FATAL_ERROR
              "TDLUA_TDLIB_CODEGEN_BUILD_DIR is not a configured TDLua build: ${_tdlua_codegen_build_dir}")
    endif()
    file(STRINGS "${_tdlua_codegen_build_dir}/CMakeCache.txt"
         _tdlua_codegen_home_line REGEX "^CMAKE_HOME_DIRECTORY:INTERNAL=")
    file(STRINGS "${_tdlua_codegen_build_dir}/CMakeCache.txt"
         _tdlua_codegen_backend_line REGEX "^TDLUA_BACKEND:STRING=")
    file(STRINGS "${_tdlua_codegen_build_dir}/CMakeCache.txt"
         _tdlua_codegen_bundled_line REGEX "^TDLUA_BUNDLED_TDLIB:BOOL=")
    file(STRINGS "${_tdlua_codegen_build_dir}/CMakeCache.txt"
         _tdlua_codegen_compiler_line
         REGEX "^CMAKE_CXX_COMPILER:(FILEPATH|STRING)=")
    string(REPLACE "CMAKE_HOME_DIRECTORY:INTERNAL=" ""
           _tdlua_codegen_home "${_tdlua_codegen_home_line}")
    string(REPLACE "TDLUA_BACKEND:STRING=" ""
           _tdlua_codegen_backend "${_tdlua_codegen_backend_line}")
    string(REPLACE "TDLUA_BUNDLED_TDLIB:BOOL=" ""
           _tdlua_codegen_bundled "${_tdlua_codegen_bundled_line}")
    string(REGEX REPLACE "^CMAKE_CXX_COMPILER:(FILEPATH|STRING)=" ""
           _tdlua_codegen_compiler "${_tdlua_codegen_compiler_line}")
    get_filename_component(_tdlua_source_dir_real "${CMAKE_CURRENT_SOURCE_DIR}" REALPATH)
    get_filename_component(_tdlua_codegen_home_real "${_tdlua_codegen_home}" REALPATH)
    get_filename_component(_tdlua_compiler_real "${CMAKE_CXX_COMPILER}" REALPATH)
    get_filename_component(_tdlua_codegen_compiler_real "${_tdlua_codegen_compiler}" REALPATH)
    if(NOT _tdlua_codegen_home_real STREQUAL _tdlua_source_dir_real
       OR NOT _tdlua_codegen_backend STREQUAL "native"
       OR NOT _tdlua_codegen_bundled STREQUAL "ON"
       OR NOT _tdlua_codegen_compiler_real STREQUAL _tdlua_compiler_real)
      message(FATAL_ERROR
              "TDLUA_TDLIB_CODEGEN_BUILD_DIR must be a bundled native build of this source tree using the same C++ compiler")
    endif()
    foreach(_tdlua_codegen_artifact
            "td/tdtl/libtdtl.a"
            "td/td/generate/tl-parser/tl-parser"
            "td/tdutils/td/utils/config.h")
      if(NOT EXISTS "${_tdlua_codegen_build_dir}/${_tdlua_codegen_artifact}")
        message(FATAL_ERROR
                "Missing TDLib codec tool artifact '${_tdlua_codegen_artifact}' in ${_tdlua_codegen_build_dir}; build the bundled native codec tools once first")
      endif()
    endforeach()
    set(_tdlua_native_codegen_library
        "${_tdlua_codegen_build_dir}/td/tdtl/libtdtl.a")
    set(_tdlua_native_tl_parser
        "${_tdlua_codegen_build_dir}/td/td/generate/tl-parser/tl-parser")
    set(_tdlua_tdjson_private_target Td::tdjson_private)
  else()
    find_package(Td CONFIG REQUIRED)
  endif()
endif()

if(TDLUA_CALLS)
  message(FATAL_ERROR "VoIP bindings have been removed; TDLUA_CALLS is no longer supported")
endif()

if(TDLUA_TDLIB_VERSION STREQUAL "")
  if(TDLUA_BUNDLED_TDLIB AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/td/CMakeLists.txt")
    file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/td/CMakeLists.txt" _tdlib_project_line
         REGEX "^project\\(TDLib VERSION ")
    string(REGEX MATCH "VERSION ([0-9]+\\.[0-9]+\\.[0-9]+)"
           _tdlib_project_match "${_tdlib_project_line}")
    if(_tdlib_project_match)
      set(TDLUA_TDLIB_VERSION "${CMAKE_MATCH_1}")
    endif()
  elseif(DEFINED TDLib_VERSION)
    set(TDLUA_TDLIB_VERSION "${TDLib_VERSION}")
  elseif(DEFINED Td_VERSION)
    set(TDLUA_TDLIB_VERSION "${Td_VERSION}")
  elseif(DEFINED Td_DIR AND EXISTS "${Td_DIR}/TdConfigVersion.cmake")
    file(STRINGS "${Td_DIR}/TdConfigVersion.cmake" _tdlib_version_line
         REGEX "^set\\(PACKAGE_VERSION ")
    string(REGEX MATCH "\"([0-9]+\\.[0-9]+\\.[0-9]+)\""
           _tdlib_version_match "${_tdlib_version_line}")
    if(_tdlib_version_match)
      set(TDLUA_TDLIB_VERSION "${CMAKE_MATCH_1}")
    endif()
  endif()
endif()
if(TDLUA_TDLIB_VERSION STREQUAL "")
  set(TDLUA_TDLIB_VERSION "unknown")
endif()
set(TDLUA_VERSION_STRING "${TDLUA_BASE_VERSION}-${TDLUA_TDLIB_VERSION}")
message(STATUS "TDLua version: ${TDLUA_VERSION_STRING}")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/tdlua-version.txt"
     "${TDLUA_VERSION_STRING}\n")

if(_tdlua_backend STREQUAL "native")
  list(APPEND tdlua_srcs
      "src/binding/client.cpp"
      "src/backend/native/runtime.cpp"
      "src/backend/native/client.cpp"
      "src/backend/native/dispatcher.cpp"
      "src/backend/native/binding.cpp"
      "src/common/lua_json.cpp"
  )
else()
  list(APPEND tdlua_srcs
      "src/binding/client.cpp"
      "src/backend/json/client.cpp"
      "src/backend/json/runtime.cpp"
      "src/common/lua_json.cpp"
      "src/backend/json/binding.cpp"
      "src/backend/json/dispatcher.cpp"
  )
endif()

if(_tdlua_backend STREQUAL "native")
  set(TDLUA_NATIVE_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated/tdlua")
  set(TDLUA_NATIVE_GENERATED_HEADER "${TDLUA_NATIVE_GENERATED_DIR}/native_codec_generated.h")
  set(TDLUA_NATIVE_SHARD_COUNT "${TDLUA_NATIVE_SHARDS}")
  set(TDLUA_NATIVE_GENERATED_PARTS)
  math(EXPR _tdlua_last_shard "${TDLUA_NATIVE_SHARDS} - 1")
  foreach(part RANGE 0 ${_tdlua_last_shard})
    list(APPEND TDLUA_NATIVE_GENERATED_PARTS
        "${TDLUA_NATIVE_GENERATED_DIR}/native_codec_${part}.cpp")
  endforeach()
  set(TDLUA_NATIVE_DISPATCH
      "${TDLUA_NATIVE_GENERATED_DIR}/native_codec_dispatch.cpp")
  if(TDLUA_BUNDLED_TDLIB)
    set(TDLUA_TD_API_TLO "${CMAKE_CURRENT_SOURCE_DIR}/td/td/generate/auto/tlo/td_api.tlo")
    set(_tdlua_native_codegen_library_target tdtl)
    set(_tdlua_native_tlo_dependency tl_generate_tlo)
  else()
    set(TDLUA_TD_API_TLO "${TDLUA_NATIVE_GENERATED_DIR}/td_api.tlo")
    add_library(tdlua_external_tdtl STATIC IMPORTED GLOBAL)
    set_target_properties(tdlua_external_tdtl PROPERTIES
        IMPORTED_LOCATION "${_tdlua_native_codegen_library}"
        INTERFACE_INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/td/tdtl")
    add_executable(tdlua_external_tl_parser IMPORTED GLOBAL)
    set_target_properties(tdlua_external_tl_parser PROPERTIES
        IMPORTED_LOCATION "${_tdlua_native_tl_parser}")
    set(_tdlua_native_codegen_library_target tdlua_external_tdtl)
    set(_tdlua_native_tl_schema
        "${CMAKE_CURRENT_SOURCE_DIR}/td/td/generate/scheme/td_api.tl")
    add_custom_command(
        OUTPUT "${TDLUA_TD_API_TLO}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${TDLUA_NATIVE_GENERATED_DIR}"
        COMMAND $<TARGET_FILE:tdlua_external_tl_parser>
                -e "${TDLUA_TD_API_TLO}" "${_tdlua_native_tl_schema}"
        DEPENDS tdlua_external_tl_parser "${_tdlua_native_tl_schema}"
        COMMENT "Generate TDLib native schema from the pinned td_api.tl"
        VERBATIM)
    add_custom_target(tdlua_native_tlo DEPENDS "${TDLUA_TD_API_TLO}")
    set(_tdlua_native_tlo_dependency tdlua_native_tlo)
  endif()

  add_executable(tdlua_native_generator src/backend/native/generator.cpp)
  target_link_libraries(tdlua_native_generator PRIVATE ${_tdlua_native_codegen_library_target})
  target_compile_features(tdlua_native_generator PRIVATE cxx_std_23)

  add_custom_command(
      OUTPUT "${TDLUA_NATIVE_GENERATED_DIR}/native_codec_generated.stamp"
      BYPRODUCTS "${TDLUA_NATIVE_GENERATED_HEADER}" ${TDLUA_NATIVE_GENERATED_PARTS}
                "${TDLUA_NATIVE_DISPATCH}"
      COMMAND ${CMAKE_COMMAND} -E make_directory "${TDLUA_NATIVE_GENERATED_DIR}"
      COMMAND tdlua_native_generator
              "${TDLUA_TD_API_TLO}"
              "${TDLUA_NATIVE_GENERATED_HEADER}"
              "${TDLUA_NATIVE_GENERATED_DIR}"
              "${TDLUA_NATIVE_SHARD_COUNT}"
      COMMAND ${CMAKE_COMMAND} -E touch "${TDLUA_NATIVE_GENERATED_DIR}/native_codec_generated.stamp"
      DEPENDS tdlua_native_generator ${_tdlua_native_tlo_dependency}
              "${CMAKE_CURRENT_SOURCE_DIR}/td/td/generate/scheme/td_api.tl"
      COMMENT "Generate sharded TDLua native TDLib codec"
      VERBATIM
  )
  add_custom_target(tdlua_native_generated
      DEPENDS "${TDLUA_NATIVE_GENERATED_DIR}/native_codec_generated.stamp")
  list(APPEND tdlua_srcs ${TDLUA_NATIVE_GENERATED_PARTS} "${TDLUA_NATIVE_DISPATCH}")
endif()

add_library (tdlua SHARED ${tdlua_srcs})

target_include_directories(tdlua PRIVATE
    ${LUA_INCLUDE_DIRS}
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)
if(_tdlua_backend STREQUAL "native")
  target_include_directories(tdlua PRIVATE "${TDLUA_NATIVE_GENERATED_DIR}")
  add_dependencies(tdlua tdlua_native_generated)
endif()
target_link_libraries(tdlua PRIVATE ${LUA_LIBRARIES})
tdlua_enable_sanitizers(tdlua)
if(_tdlua_backend STREQUAL "native")
  target_compile_features(tdlua PRIVATE cxx_std_23)
  if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(tdlua PRIVATE -fno-sanitize=vptr)
  endif()
else()
  target_compile_features(tdlua PRIVATE cxx_std_11)
endif()
target_compile_definitions(tdlua PRIVATE
    "TDLUA_VERSION_STRING=\"${TDLUA_VERSION_STRING}\""
    "TDLUA_BASE_VERSION=\"${TDLUA_BASE_VERSION}\""
    "TDLUA_TDLIB_VERSION=\"${TDLUA_TDLIB_VERSION}\"")
if(_tdlua_backend STREQUAL "json")
  target_compile_definitions(tdlua PRIVATE TDLUA_JSON_BACKEND)
else()
  target_compile_definitions(tdlua PRIVATE TDLUA_NATIVE_BACKEND)
endif()


set_target_properties(tdlua PROPERTIES PREFIX "")
if(_tdlua_backend STREQUAL "native")
  # The native backend is tied to the TDLib C++ ABI and is always static.
  target_link_libraries(tdlua PRIVATE Td::TdStatic)
elseif(_tdlua_json_static)
  target_link_libraries(tdlua PRIVATE Td::TdJsonStatic)
else()
  # The JSON backend uses TDLib's stable C ABI and is replaceable at runtime.
  target_link_libraries(tdlua PRIVATE Td::TdJson)
endif()
if(_tdlua_backend STREQUAL "json" OR _tdlua_backend STREQUAL "native")
  # The threaded runtime's residency contract is reviewed only on this platform.
  if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
    message(FATAL_ERROR "Threaded TDLua currently requires Linux x86_64/glibc")
  endif()
  include(CheckCSourceCompiles)
  check_c_source_compiles("#include <features.h>\n#ifndef __GLIBC__\n#error glibc required\n#endif\nint main(void) { return 0; }" TDLUA_GLIBC)
  if(NOT TDLUA_GLIBC)
    message(FATAL_ERROR "Threaded TDLua requires a glibc ELF loader with NODELETE")
  endif()
  find_package(Threads REQUIRED)
  target_link_libraries(tdlua PRIVATE Threads::Threads)
  target_link_options(tdlua PRIVATE -Wl,-z,nodelete)
endif()
if(TDLUA_BUNDLED_TDLIB AND UNIX AND _tdlua_backend STREQUAL "json"
   AND NOT _tdlua_json_static)
  if(APPLE)
    set_property(TARGET tdlua APPEND PROPERTY INSTALL_RPATH
        "@loader_path;@loader_path/../..")
  else()
    set_property(TARGET tdlua APPEND PROPERTY INSTALL_RPATH
        "$ORIGIN;$ORIGIN/../..")
  endif()
endif()

if(NOT TDLUA_LUA_VERSION)
  if(LUA_VERSION_STRING)
    string(REGEX MATCH "^[0-9]+\\.[0-9]+" TDLUA_LUA_VERSION_MATCH
           "${LUA_VERSION_STRING}")
    if(TDLUA_LUA_VERSION_MATCH)
      set(TDLUA_LUA_VERSION "${TDLUA_LUA_VERSION_MATCH}")
    else()
      set(TDLUA_LUA_VERSION "${LUA_VERSION_STRING}")
    endif()
  else()
    string(REGEX MATCH "lua([0-9]+\\.[0-9]+)" TDLUA_LUA_VERSION_MATCH
           "${LUA_INCLUDE_DIR}")
    if(CMAKE_MATCH_1)
      set(TDLUA_LUA_VERSION "${CMAKE_MATCH_1}")
    else()
      set(TDLUA_LUA_VERSION "generic")
    endif()
  endif()
endif()
if(TDLUA_LUA_MODULE_DIR STREQUAL "")
    set(TDLUA_LUA_MODULE_DIR
        "${CMAKE_INSTALL_LIBDIR}/lua/${TDLUA_LUA_VERSION}"
        CACHE STRING "Installation directory for the tdlua Lua module" FORCE)
endif()

include(CMakePackageConfigHelpers)
configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/tdluaConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/tdluaConfig.cmake"
    INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/tdlua"
)
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/tdluaConfigVersion.cmake"
    VERSION "${PROJECT_VERSION}"
    COMPATIBILITY SameMajorVersion
)

install(TARGETS tdlua
    EXPORT TDLuaTargets
    LIBRARY DESTINATION "${TDLUA_LUA_MODULE_DIR}"
    RUNTIME DESTINATION "${TDLUA_LUA_MODULE_DIR}"
)
install(EXPORT TDLuaTargets
    FILE TDLuaTargets.cmake
    NAMESPACE TDLua::
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/tdlua"
)
install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/tdluaConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/tdluaConfigVersion.cmake"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/tdlua"
)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/tdlua")
if(TDLUA_BUNDLED_TDLIB)
  install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/td/LICENSE_1_0.txt"
      DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/tdlua"
      RENAME "TDLib-LICENSE_1_0.txt")
endif()

if(TDLUA_BUILD_TESTS)
    enable_testing()
    add_executable(tdlua_core_anchor_ownership_test "tests/core_anchor_ownership_test.cpp")
    target_include_directories(tdlua_core_anchor_ownership_test PRIVATE
        ${LUA_INCLUDE_DIRS} "${CMAKE_CURRENT_SOURCE_DIR}/include" "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(tdlua_core_anchor_ownership_test PRIVATE
        ${LUA_LIBRARIES} tdlua_lua_runtime)
    target_compile_features(tdlua_core_anchor_ownership_test PRIVATE cxx_std_11)
    tdlua_enable_sanitizers(tdlua_core_anchor_ownership_test)
    add_test(NAME tdlua_core_anchor_ownership COMMAND tdlua_core_anchor_ownership_test)
    add_executable(tdlua_managed_routing_oom_test "tests/managed_routing_oom_test.cpp")
    target_compile_definitions(tdlua_managed_routing_oom_test PRIVATE TDLUA_TESTING)
    target_include_directories(tdlua_managed_routing_oom_test PRIVATE
        ${LUA_INCLUDE_DIRS} "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_link_libraries(tdlua_managed_routing_oom_test PRIVATE
        ${LUA_LIBRARIES} tdlua_lua_runtime)
    target_compile_features(tdlua_managed_routing_oom_test PRIVATE cxx_std_11)
    tdlua_enable_sanitizers(tdlua_managed_routing_oom_test)
    add_test(NAME tdlua_managed_routing_oom COMMAND tdlua_managed_routing_oom_test)
    add_executable(tdlua_json_push_oom_test
        "tests/json_push_oom_test.cpp" "src/common/lua_json.cpp")
    target_include_directories(tdlua_json_push_oom_test PRIVATE
        ${LUA_INCLUDE_DIRS} "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_link_libraries(tdlua_json_push_oom_test PRIVATE
        ${LUA_LIBRARIES} tdlua_lua_runtime)
    target_compile_features(tdlua_json_push_oom_test PRIVATE cxx_std_11)
    tdlua_enable_sanitizers(tdlua_json_push_oom_test)
    add_test(NAME tdlua_json_push_oom COMMAND tdlua_json_push_oom_test)
    add_executable(tdlua_client_pump_test "tests/client_pump_test.cpp")
    target_include_directories(tdlua_client_pump_test PRIVATE
        ${LUA_INCLUDE_DIRS} "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_link_libraries(tdlua_client_pump_test PRIVATE
        ${LUA_LIBRARIES} tdlua_lua_runtime)
    target_compile_features(tdlua_client_pump_test PRIVATE cxx_std_11)
    target_compile_options(tdlua_client_pump_test PRIVATE -Wall -Wextra -Wpedantic)
    tdlua_enable_sanitizers(tdlua_client_pump_test)
    add_test(NAME tdlua_client_pump COMMAND tdlua_client_pump_test)

    add_executable(tdlua_lua_protected_test "tests/lua_protected_test.cpp")
    target_include_directories(tdlua_lua_protected_test PRIVATE
        ${LUA_INCLUDE_DIRS} "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_link_libraries(tdlua_lua_protected_test PRIVATE
        ${LUA_LIBRARIES} tdlua_lua_runtime Threads::Threads)
    target_compile_features(tdlua_lua_protected_test PRIVATE cxx_std_11)
    target_compile_definitions(tdlua_lua_protected_test PRIVATE TDLUA_TESTING)
    tdlua_enable_sanitizers(tdlua_lua_protected_test)
    add_test(NAME tdlua_lua_protected COMMAND tdlua_lua_protected_test)

    if(_tdlua_backend STREQUAL "json" OR _tdlua_backend STREQUAL "native")
        add_executable(tdlua_shutdown_admission_test "tests/shutdown_admission_test.cpp")
        target_include_directories(tdlua_shutdown_admission_test PRIVATE
            ${LUA_INCLUDE_DIRS} "${CMAKE_CURRENT_SOURCE_DIR}/include")
        target_link_libraries(tdlua_shutdown_admission_test PRIVATE
            ${LUA_LIBRARIES} tdlua_lua_runtime ${CMAKE_DL_LIBS})
        target_compile_features(tdlua_shutdown_admission_test PRIVATE cxx_std_11)
        tdlua_enable_sanitizers(tdlua_shutdown_admission_test)
        add_test(NAME tdlua_shutdown_admission COMMAND tdlua_shutdown_admission_test
            "$<TARGET_FILE_DIR:tdlua>" ${_tdlua_backend})
        add_executable(tdlua_receiver_runtime_test tests/receiver_runtime_test.cpp)
        target_include_directories(tdlua_receiver_runtime_test PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/include")
        target_link_libraries(tdlua_receiver_runtime_test PRIVATE Threads::Threads)
        target_compile_definitions(tdlua_receiver_runtime_test PRIVATE TDLUA_TESTING)
        set_target_properties(tdlua_receiver_runtime_test PROPERTIES
            CXX_STANDARD 11
            CXX_STANDARD_REQUIRED YES)
        tdlua_enable_sanitizers(tdlua_receiver_runtime_test)
        add_test(NAME tdlua_receiver_runtime COMMAND tdlua_receiver_runtime_test)
        add_test(NAME tdlua_receiver_generic_fifo_metadata
            COMMAND tdlua_receiver_runtime_test --generic-fifo-metadata-only)

        add_executable(tdlua_buffering_accounting_test
            tests/buffering_accounting_test.cpp)
        target_include_directories(tdlua_buffering_accounting_test PRIVATE
            ${LUA_INCLUDE_DIRS} "${CMAKE_CURRENT_SOURCE_DIR}/include")
        target_link_libraries(tdlua_buffering_accounting_test PRIVATE
            ${LUA_LIBRARIES} tdlua_lua_runtime Threads::Threads)
        target_compile_definitions(tdlua_buffering_accounting_test PRIVATE
            TDLUA_TESTING)
        target_compile_features(tdlua_buffering_accounting_test PRIVATE cxx_std_11)
        tdlua_enable_sanitizers(tdlua_buffering_accounting_test)
        add_test(NAME tdlua_buffering_accounting
            COMMAND tdlua_buffering_accounting_test 10000)
        set_tests_properties(tdlua_buffering_accounting PROPERTIES TIMEOUT 120)

        add_executable(tdlua_scheduler_concurrency_test
            tests/scheduler_concurrency_test.cpp)
        target_include_directories(tdlua_scheduler_concurrency_test PRIVATE
            ${LUA_INCLUDE_DIRS} "${CMAKE_CURRENT_SOURCE_DIR}/include")
        target_link_libraries(tdlua_scheduler_concurrency_test PRIVATE
            ${LUA_LIBRARIES} tdlua_lua_runtime Threads::Threads)
        target_compile_definitions(tdlua_scheduler_concurrency_test PRIVATE
            TDLUA_TESTING)
        target_compile_features(tdlua_scheduler_concurrency_test PRIVATE cxx_std_11)
        tdlua_enable_sanitizers(tdlua_scheduler_concurrency_test)
        add_test(NAME tdlua_scheduler_concurrency
            COMMAND tdlua_scheduler_concurrency_test)
        set_tests_properties(tdlua_scheduler_concurrency PROPERTIES TIMEOUT 60)

        add_executable(tdlua_retention_policy_poc_test
            tests/retention_policy_poc_test.cpp)
        target_compile_features(tdlua_retention_policy_poc_test PRIVATE cxx_std_11)
        tdlua_enable_sanitizers(tdlua_retention_policy_poc_test)
        add_test(NAME tdlua_retention_policy_poc
            COMMAND tdlua_retention_policy_poc_test)
    endif()
    target_compile_definitions(tdlua PRIVATE TDLUA_TESTING)
    add_executable(tdlua_codec_test
        "tests/codec_test.cpp"
        "src/common/lua_json.cpp"
    )
    target_include_directories(tdlua_codec_test PRIVATE
        ${LUA_INCLUDE_DIRS}
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
    )
    target_link_libraries(tdlua_codec_test PRIVATE ${LUA_LIBRARIES})
    tdlua_enable_sanitizers(tdlua_codec_test)
    set_property(TARGET tdlua_codec_test PROPERTY CXX_STANDARD 11)
    add_test(NAME tdlua_codec COMMAND tdlua_codec_test)

    if(NOT TDLUA_USE_LUA51_CONTINUATION)
        add_executable(tdlua_managed_adapter_test "tests/managed_adapter_test.cpp")
        target_include_directories(tdlua_managed_adapter_test PRIVATE
            ${LUA_INCLUDE_DIRS} "${CMAKE_CURRENT_SOURCE_DIR}/include")
        target_link_libraries(tdlua_managed_adapter_test PRIVATE ${LUA_LIBRARIES})
        target_compile_features(tdlua_managed_adapter_test PRIVATE cxx_std_11)
        target_compile_definitions(tdlua_managed_adapter_test PRIVATE TDLUA_TESTING)
        tdlua_enable_sanitizers(tdlua_managed_adapter_test)
        add_test(NAME tdlua_managed_adapter COMMAND tdlua_managed_adapter_test)
        if(TDLUA_USE_LUAJIT_CONTINUATION)
            add_test(NAME tdlua_managed_adapter_jit_off COMMAND tdlua_managed_adapter_test --jit-off)

            add_executable(tdlua_managed_cache_test "tests/managed_cache_test.cpp")
            target_include_directories(tdlua_managed_cache_test PRIVATE
                ${LUA_INCLUDE_DIRS} "${CMAKE_CURRENT_SOURCE_DIR}/include"
                "${CMAKE_CURRENT_SOURCE_DIR}/src")
            target_link_libraries(tdlua_managed_cache_test PRIVATE tdlua ${LUA_LIBRARIES})
            target_compile_features(tdlua_managed_cache_test PRIVATE cxx_std_11)
            target_compile_definitions(tdlua_managed_cache_test PRIVATE TDLUA_TESTING)
            tdlua_enable_sanitizers(tdlua_managed_cache_test)
            add_test(NAME tdlua_managed_cache COMMAND tdlua_managed_cache_test)
            add_test(NAME tdlua_managed_cache_jit_off
                COMMAND tdlua_managed_cache_test --jit-off)
        endif()

        add_executable(tdlua_common_router_test "tests/common_router_test.cpp")
        target_include_directories(tdlua_common_router_test PRIVATE
            ${LUA_INCLUDE_DIRS} "${CMAKE_CURRENT_SOURCE_DIR}/include")
        target_link_libraries(tdlua_common_router_test PRIVATE ${LUA_LIBRARIES})
        target_compile_features(tdlua_common_router_test PRIVATE cxx_std_11)
        tdlua_enable_sanitizers(tdlua_common_router_test)
        add_test(NAME tdlua_common_router COMMAND tdlua_common_router_test)

        add_executable(tdlua_hardening_common_test "tests/hardening_common_test.cpp")
        target_include_directories(tdlua_hardening_common_test PRIVATE
            ${LUA_INCLUDE_DIRS} "${CMAKE_CURRENT_SOURCE_DIR}/include")
        target_link_libraries(tdlua_hardening_common_test PRIVATE ${LUA_LIBRARIES})
        target_compile_features(tdlua_hardening_common_test PRIVATE cxx_std_11)
        tdlua_enable_sanitizers(tdlua_hardening_common_test)
        add_test(NAME tdlua_hardening_common COMMAND tdlua_hardening_common_test)

        add_executable(tdlua_managed_driver_test
            "tests/managed_driver_test.cpp"
        )
        target_include_directories(tdlua_managed_driver_test PRIVATE
            ${LUA_INCLUDE_DIRS} "${CMAKE_CURRENT_SOURCE_DIR}/include")
        target_link_libraries(tdlua_managed_driver_test PRIVATE ${LUA_LIBRARIES})
        target_compile_features(tdlua_managed_driver_test PRIVATE cxx_std_11)
        tdlua_enable_sanitizers(tdlua_managed_driver_test)
        add_test(NAME tdlua_managed_driver_common
            COMMAND tdlua_managed_driver_test
        )
    endif()

    if(_tdlua_backend STREQUAL "json" AND NOT TDLUA_USE_LUA51_CONTINUATION)
        add_executable(tdlua_fake_client_test "tests/fake_client_test.cpp")
        target_include_directories(tdlua_fake_client_test PRIVATE
            ${LUA_INCLUDE_DIRS} "${CMAKE_CURRENT_SOURCE_DIR}/include"
            "${CMAKE_CURRENT_SOURCE_DIR}/src")
        target_compile_definitions(tdlua_fake_client_test PRIVATE TDLUA_TESTING)
        target_compile_features(tdlua_fake_client_test PRIVATE cxx_std_11)
        target_link_libraries(tdlua_fake_client_test PRIVATE tdlua ${LUA_LIBRARIES})
        tdlua_enable_sanitizers(tdlua_fake_client_test)
        add_test(NAME tdlua_fake_client COMMAND tdlua_fake_client_test)
        add_test(NAME tdlua_terminal_shutdown
                 COMMAND tdlua_fake_client_test --terminal-close-only)
        add_test(NAME tdlua_harness_cleanup COMMAND tdlua_fake_client_test --harness-failure-only)
        add_test(NAME tdlua_hardening_fake
            COMMAND tdlua_fake_client_test --hardening-only)
        add_test(NAME tdlua_handler_only_prefilter
            COMMAND tdlua_fake_client_test --handler-only-prefilter)
        add_test(NAME tdlua_handler_mutation
            COMMAND tdlua_fake_client_test --handler-mutation-only)
        add_test(NAME tdlua_managed_driver
            COMMAND tdlua_fake_client_test --m4-only
        )
        add_test(NAME tdlua_m5_conformance
            COMMAND tdlua_fake_client_test --m5-only
        )

        add_executable(tdlua_simple_bot_example_test
            "tests/simple_bot_example_test.cpp"
        )
        target_include_directories(tdlua_simple_bot_example_test PRIVATE
            ${LUA_INCLUDE_DIRS} "${CMAKE_CURRENT_SOURCE_DIR}/include"
            "${CMAKE_CURRENT_SOURCE_DIR}/src")
        target_compile_definitions(tdlua_simple_bot_example_test PRIVATE
            TDLUA_TESTING
            TDLUA_SIMPLE_BOT_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
        target_compile_features(tdlua_simple_bot_example_test PRIVATE cxx_std_11)
        target_link_libraries(tdlua_simple_bot_example_test PRIVATE
            tdlua ${LUA_LIBRARIES})
        tdlua_enable_sanitizers(tdlua_simple_bot_example_test)
        add_test(NAME tdlua_simple_bot_example
            COMMAND tdlua_simple_bot_example_test
        )

        add_executable(tdlua_async_demo_test
            "tests/async_demo_test.cpp"
        )
        target_include_directories(tdlua_async_demo_test PRIVATE
            ${LUA_INCLUDE_DIRS} "${CMAKE_CURRENT_SOURCE_DIR}/include"
            "${CMAKE_CURRENT_SOURCE_DIR}/src")
        target_compile_definitions(tdlua_async_demo_test PRIVATE
            TDLUA_TESTING
            TDLUA_ASYNC_DEMO_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
        target_compile_features(tdlua_async_demo_test PRIVATE cxx_std_11)
        target_link_libraries(tdlua_async_demo_test PRIVATE
            tdlua ${LUA_LIBRARIES})
        tdlua_enable_sanitizers(tdlua_async_demo_test)
        add_test(NAME tdlua_async_demo
            COMMAND tdlua_async_demo_test
        )
    endif()

    if(TDLUA_USE_LUA51_CONTINUATION)
        add_executable(tdlua_lua51_wait_fault_test "tests/lua51_wait_fault_test.cpp")
        target_include_directories(tdlua_lua51_wait_fault_test PRIVATE
            ${LUA_INCLUDE_DIRS} "${CMAKE_CURRENT_SOURCE_DIR}/include")
        target_link_libraries(tdlua_lua51_wait_fault_test PRIVATE ${LUA_LIBRARIES})
        target_compile_features(tdlua_lua51_wait_fault_test PRIVATE cxx_std_11)
        target_compile_definitions(tdlua_lua51_wait_fault_test PRIVATE TDLUA_TESTING)
        tdlua_enable_sanitizers(tdlua_lua51_wait_fault_test)
        add_test(NAME tdlua_lua51_wait_fault COMMAND tdlua_lua51_wait_fault_test)
        add_executable(tdlua_lua51_managed_test "tests/lua51_managed_test.cpp")
        target_include_directories(tdlua_lua51_managed_test PRIVATE
            ${LUA_INCLUDE_DIRS} "${CMAKE_CURRENT_SOURCE_DIR}/include"
            "${CMAKE_CURRENT_SOURCE_DIR}/src")
        target_compile_definitions(tdlua_lua51_managed_test PRIVATE TDLUA_TESTING
            TDLUA_LUA51_SCRIPT="${CMAKE_CURRENT_SOURCE_DIR}/tests/lua51_managed.lua")
        target_compile_features(tdlua_lua51_managed_test PRIVATE cxx_std_11)
        target_link_libraries(tdlua_lua51_managed_test PRIVATE tdlua ${LUA_LIBRARIES})
        if(_tdlua_backend STREQUAL "native")
            target_compile_definitions(tdlua_lua51_managed_test PRIVATE TDLUA_NATIVE_BACKEND)
            target_include_directories(tdlua_lua51_managed_test PRIVATE
                "${CMAKE_CURRENT_SOURCE_DIR}/td" "${CMAKE_CURRENT_SOURCE_DIR}/td/td/generate/auto")
            target_compile_features(tdlua_lua51_managed_test PRIVATE cxx_std_23)
        endif()
        tdlua_enable_sanitizers(tdlua_lua51_managed_test)
        add_test(NAME tdlua_lua51_managed COMMAND tdlua_lua51_managed_test)
        set_tests_properties(tdlua_lua51_managed PROPERTIES TIMEOUT 30)
    endif()

    find_package(Threads REQUIRED)
    add_executable(tdlua_vm_threads_test "tests/vm_threads_test.cpp")
    target_include_directories(tdlua_vm_threads_test PRIVATE ${LUA_INCLUDE_DIRS}
        "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_link_libraries(tdlua_vm_threads_test PRIVATE ${LUA_LIBRARIES}
        tdlua_lua_runtime Threads::Threads)
    target_compile_features(tdlua_vm_threads_test PRIVATE cxx_std_11)
    tdlua_enable_sanitizers(tdlua_vm_threads_test)
    add_test(NAME tdlua_vm_threads COMMAND tdlua_vm_threads_test)
    set_tests_properties(tdlua_vm_threads PROPERTIES TIMEOUT 30)

    add_executable(tdlua_release_test "tests/release_test.cpp")
    target_include_directories(tdlua_release_test PRIVATE ${LUA_INCLUDE_DIRS}
        "${CMAKE_CURRENT_SOURCE_DIR}/include" "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(tdlua_release_test PRIVATE tdlua ${LUA_LIBRARIES} tdlua_lua_runtime)
    target_compile_definitions(tdlua_release_test PRIVATE TDLUA_TESTING)
    target_compile_features(tdlua_release_test PRIVATE cxx_std_11)
    if(_tdlua_backend STREQUAL "native")
        target_compile_definitions(tdlua_release_test PRIVATE TDLUA_NATIVE_BACKEND)
        target_include_directories(tdlua_release_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/td"
            "${CMAKE_CURRENT_SOURCE_DIR}/td/td/generate/auto")
        target_compile_features(tdlua_release_test PRIVATE cxx_std_23)
    endif()
    tdlua_enable_sanitizers(tdlua_release_test)
    add_test(NAME tdlua_release COMMAND tdlua_release_test)
    set_tests_properties(tdlua_release PROPERTIES TIMEOUT 30)
    if(TDLUA_USE_LUAJIT_CONTINUATION)
        add_test(NAME tdlua_release_jit_off COMMAND tdlua_release_test --jit-off)
        add_test(NAME tdlua_vm_threads_jit_off COMMAND tdlua_vm_threads_test --jit-off)
        set_tests_properties(tdlua_release_jit_off tdlua_vm_threads_jit_off PROPERTIES TIMEOUT 30)
    endif()

    if(_tdlua_backend STREQUAL "native")
        add_executable(tdlua_native_codec_test
            "tests/native_codec_test.cpp"
        )
        target_include_directories(tdlua_native_codec_test PRIVATE
            ${LUA_INCLUDE_DIRS}
            "${CMAKE_CURRENT_SOURCE_DIR}/include"
            "${CMAKE_CURRENT_SOURCE_DIR}/td"
            "${CMAKE_CURRENT_SOURCE_DIR}/td/td/generate/auto"
            "${TDLUA_NATIVE_GENERATED_DIR}"
        )
        if(NOT TDLUA_BUNDLED_TDLIB)
            target_include_directories(tdlua_native_codec_test PRIVATE
                "${CMAKE_CURRENT_SOURCE_DIR}/td/tdutils"
                "${_tdlua_codegen_build_dir}/td/tdutils")
        endif()
        target_link_libraries(tdlua_native_codec_test PRIVATE tdlua ${LUA_LIBRARIES})
        target_link_libraries(tdlua_native_codec_test PRIVATE
            ${_tdlua_tdjson_private_target})
        target_compile_features(tdlua_native_codec_test PRIVATE cxx_std_23)
        tdlua_enable_sanitizers(tdlua_native_codec_test)
        if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
            target_compile_options(tdlua_native_codec_test PRIVATE -fno-sanitize=vptr)
        endif()
        add_dependencies(tdlua_native_codec_test tdlua_native_generated)
        add_test(NAME tdlua_native_codec COMMAND tdlua_native_codec_test)

        add_executable(tdlua_native_decoder_safety_test
            "tests/native_decoder_safety_test.cpp"
        )
        target_include_directories(tdlua_native_decoder_safety_test PRIVATE
            ${LUA_INCLUDE_DIRS}
            "${CMAKE_CURRENT_SOURCE_DIR}/include"
            "${CMAKE_CURRENT_SOURCE_DIR}/td"
            "${CMAKE_CURRENT_SOURCE_DIR}/td/td/generate/auto"
            "${TDLUA_NATIVE_GENERATED_DIR}"
        )
        target_link_libraries(tdlua_native_decoder_safety_test PRIVATE
            tdlua ${LUA_LIBRARIES})
        target_compile_features(tdlua_native_decoder_safety_test PRIVATE cxx_std_23)
        tdlua_enable_sanitizers(tdlua_native_decoder_safety_test)
        if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
            target_compile_options(tdlua_native_decoder_safety_test PRIVATE
                -fno-sanitize=vptr)
        endif()
        add_dependencies(tdlua_native_decoder_safety_test tdlua_native_generated)
        add_test(NAME tdlua_native_decoder_safety
                 COMMAND tdlua_native_decoder_safety_test)

        add_executable(tdlua_native_generic_ticket_test
            "tests/native_generic_ticket_test.cpp")
        target_include_directories(tdlua_native_generic_ticket_test PRIVATE
            ${LUA_INCLUDE_DIRS}
            "${CMAKE_CURRENT_SOURCE_DIR}/include"
            "${CMAKE_CURRENT_SOURCE_DIR}/td"
            "${CMAKE_CURRENT_SOURCE_DIR}/td/td/generate/auto"
            "${TDLUA_NATIVE_GENERATED_DIR}")
        if(NOT TDLUA_BUNDLED_TDLIB)
            target_include_directories(tdlua_native_generic_ticket_test PRIVATE
                "${CMAKE_CURRENT_SOURCE_DIR}/td/tdutils"
                "${_tdlua_codegen_build_dir}/td/tdutils")
        endif()
        target_link_libraries(tdlua_native_generic_ticket_test PRIVATE
            tdlua ${LUA_LIBRARIES} tdlua_lua_runtime
            ${_tdlua_tdjson_private_target})
        target_compile_features(tdlua_native_generic_ticket_test PRIVATE cxx_std_23)
        target_compile_definitions(tdlua_native_generic_ticket_test PRIVATE
            TDLUA_TESTING TDLUA_NATIVE_BACKEND)
        tdlua_enable_sanitizers(tdlua_native_generic_ticket_test)
        if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
            target_compile_options(tdlua_native_generic_ticket_test PRIVATE
                -fno-sanitize=vptr)
        endif()
        add_dependencies(tdlua_native_generic_ticket_test tdlua_native_generated)
        add_test(NAME tdlua_native_generic_ticket
            COMMAND tdlua_native_generic_ticket_test)

        add_executable(tdlua_native_schema_test
            "tests/native_schema_test.cpp"
        )
        target_link_libraries(tdlua_native_schema_test PRIVATE
            ${_tdlua_native_codegen_library_target})
        target_compile_features(tdlua_native_schema_test PRIVATE cxx_std_23)
        tdlua_enable_sanitizers(tdlua_native_schema_test)
        if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
            target_compile_options(tdlua_native_schema_test PRIVATE -fno-sanitize=vptr)
        endif()
        add_dependencies(tdlua_native_schema_test tdlua_native_generated)
        add_test(NAME tdlua_native_schema
            COMMAND tdlua_native_schema_test
                    "${TDLUA_TD_API_TLO}"
                    "${TDLUA_NATIVE_GENERATED_DIR}"
                    "${TDLUA_NATIVE_SHARD_COUNT}")
    endif()

    add_executable(tdlua_model_benchmark "tests/model_benchmark.cpp")
    target_include_directories(tdlua_model_benchmark PRIVATE
        ${LUA_INCLUDE_DIRS}
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
    )
    target_link_libraries(tdlua_model_benchmark PRIVATE ${LUA_LIBRARIES})
    tdlua_enable_sanitizers(tdlua_model_benchmark)
    set_property(TARGET tdlua_model_benchmark PROPERTY CXX_STANDARD 11)

    # TDLUA_LUA_EXECUTABLE is an explicit test-harness override. Preserve it
    # when FindLua also reports an interpreter from a different runtime.
    if(NOT TDLUA_LUA_EXECUTABLE AND LUA_EXECUTABLE)
        set(TDLUA_LUA_EXECUTABLE "${LUA_EXECUTABLE}")
    elseif(NOT TDLUA_LUA_EXECUTABLE)
        find_program(TDLUA_LUA_EXECUTABLE
            NAMES "lua${TDLUA_LUA_VERSION}" lua5.5 lua5.4 lua5.3 lua5.2 lua5.1 luajit lua)
    endif()
    if(TDLUA_LUA_EXECUTABLE AND TDLUA_USE_LUA51_CONTINUATION)
        add_test(NAME tdlua_lua51_backend
            COMMAND ${TDLUA_LUA_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tests/lua51_backend.lua"
                "$<TARGET_FILE_DIR:tdlua>")
        set_tests_properties(tdlua_lua51_backend PROPERTIES TIMEOUT 30)
        tdlua_enable_sanitizer_test_runtime(tdlua_lua51_backend)
    elseif(TDLUA_LUA_EXECUTABLE)
        if(TDLUA_USE_LUAJIT_CONTINUATION)
            foreach(mode on off)
                add_test(NAME tdlua_luajit_${mode}
                    COMMAND ${TDLUA_LUA_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tests/luajit.lua"
                        "$<TARGET_FILE_DIR:tdlua>" ${mode})
                set_tests_properties(tdlua_luajit_${mode} PROPERTIES TIMEOUT 30)
                tdlua_enable_sanitizer_test_runtime(tdlua_luajit_${mode})
            endforeach()
        endif()
        add_test(NAME tdlua_hardening_backend
            COMMAND ${TDLUA_LUA_EXECUTABLE}
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/hardening.lua"
                    "$<TARGET_FILE_DIR:tdlua>")
        set_tests_properties(tdlua_hardening_backend PROPERTIES TIMEOUT 30)
        tdlua_enable_sanitizer_test_runtime(tdlua_hardening_backend)
        add_test(NAME tdlua_lifecycle
            COMMAND ${TDLUA_LUA_EXECUTABLE}
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/lifecycle.lua"
                    "$<TARGET_FILE_DIR:tdlua>"
        )
        add_test(NAME tdlua_async
            COMMAND ${TDLUA_LUA_EXECUTABLE}
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/async.lua"
                    "$<TARGET_FILE_DIR:tdlua>"
        )
        add_test(NAME tdlua_backend_parity
            COMMAND ${TDLUA_LUA_EXECUTABLE}
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/backend_parity.lua"
                    "$<TARGET_FILE_DIR:tdlua>"
        )
        add_test(NAME tdlua_e2e_smoke
            COMMAND ${TDLUA_LUA_EXECUTABLE}
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/e2e_smoke.lua"
                    "$<TARGET_FILE_DIR:tdlua>"
        )
        add_test(NAME tdlua_managed_driver_backend
            COMMAND ${TDLUA_LUA_EXECUTABLE}
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/managed_driver.lua"
                    "$<TARGET_FILE_DIR:tdlua>"
        )
        set_tests_properties(tdlua_managed_driver_backend PROPERTIES TIMEOUT 30)
        tdlua_enable_sanitizer_test_runtime(
            tdlua_lifecycle
            tdlua_async
            tdlua_backend_parity
            tdlua_e2e_smoke
            tdlua_managed_driver_backend
        )
    endif()
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND TDLUA_LUA_EXECUTABLE
       AND NOT TDLUA_ENABLE_SANITIZERS)
        find_package(Python3 COMPONENTS Interpreter QUIET)
        if(Python3_Interpreter_FOUND)
            add_test(NAME tdlua_signals
                COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tests/signals.py"
                    "${TDLUA_LUA_EXECUTABLE}" "$<TARGET_FILE_DIR:tdlua>")
            set_tests_properties(tdlua_signals PROPERTIES TIMEOUT 15)
        endif()
    endif()

endif()

# Apply the VM capability only to TDLua code. TDLib remains untouched.
foreach(target tdlua tdlua_userdata_constructor_test tdlua_lua51_wait_fault_test tdlua_lua51_managed_test tdlua_codec_test tdlua_managed_adapter_test tdlua_managed_cache_test
        tdlua_common_router_test
        tdlua_hardening_common_test tdlua_managed_driver_test tdlua_fake_client_test
        tdlua_simple_bot_example_test tdlua_async_demo_test
        tdlua_native_codec_test tdlua_native_decoder_safety_test
        tdlua_model_benchmark)
    if(TARGET ${target})
        target_link_libraries(${target} PRIVATE tdlua_lua_runtime)
    endif()
endforeach()
