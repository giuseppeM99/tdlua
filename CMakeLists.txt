cmake_minimum_required(VERSION 3.20)

file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/dist.info" _tdlua_dist_version_line
     REGEX "^version[ \t]*=")
string(REGEX MATCH "\"([0-9]+\\.[0-9]+)\""
       _tdlua_dist_version_match "${_tdlua_dist_version_line}")
if(NOT _tdlua_dist_version_match)
  message(FATAL_ERROR "Unable to read TDLua version from dist.info")
endif()
set(_tdlua_base_version "${CMAKE_MATCH_1}")
project(tdlua VERSION "${_tdlua_base_version}.0" LANGUAGES C CXX)

set(CMAKE_POSITION_INDEPENDENT_CODE ON)
# Keep the compilation database enabled for clangd and static analysis. This
# is a project setting rather than a user-selectable backend option.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(TDLUA_BASE_VERSION "${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}")
option(TDLUA_BUILD_TESTS "Build TDLua regression tests" ON)
option(TDLUA_BUNDLED_TDLIB "Build the TDLib copy in the td subdirectory" ON)
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

if(_tdlua_native_backend AND NOT TDLUA_BUNDLED_TDLIB)
  message(FATAL_ERROR
          "TDLUA_BACKEND=native requires TDLUA_BUNDLED_TDLIB=ON because the native codec is generated from TDLib's schema")
endif()

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
else()
  find_package(Td CONFIG REQUIRED)
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
      "src/native_runtime.cpp"
      "src/native_tdlua.cpp"
      "src/native_dispatcher.cpp"
      "src/binding/native_client.cpp"
      "src/luajson.cpp"
  )
else()
  list(APPEND tdlua_srcs
      "src/tdlua.cpp"
      "src/luajson.cpp"
      "src/binding/json_client.cpp"
      "src/request_router.cpp"
      "src/lua_dispatcher.cpp"
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
  set(TDLUA_TD_API_TLO "${CMAKE_CURRENT_SOURCE_DIR}/td/td/generate/auto/tlo/td_api.tlo")

  add_executable(tdlua_native_generator src/native_generator.cpp)
  target_link_libraries(tdlua_native_generator PRIVATE tdtl)
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
      DEPENDS tdlua_native_generator tl_generate_tlo
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

if(TDLUA_BUILD_TESTS)
    enable_testing()
    target_compile_definitions(tdlua PRIVATE TDLUA_TESTING)
    add_executable(tdlua_codec_test
        "tests/codec_test.cpp"
        "src/luajson.cpp"
    )
    target_include_directories(tdlua_codec_test PRIVATE
        ${LUA_INCLUDE_DIRS}
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
    )
    target_link_libraries(tdlua_codec_test PRIVATE ${LUA_LIBRARIES})
    tdlua_enable_sanitizers(tdlua_codec_test)
    set_property(TARGET tdlua_codec_test PROPERTY CXX_STANDARD 11)
    add_test(NAME tdlua_codec COMMAND tdlua_codec_test)

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
        target_link_libraries(tdlua_native_codec_test PRIVATE tdlua ${LUA_LIBRARIES})
        target_compile_features(tdlua_native_codec_test PRIVATE cxx_std_23)
        tdlua_enable_sanitizers(tdlua_native_codec_test)
        if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
            target_compile_options(tdlua_native_codec_test PRIVATE -fno-sanitize=vptr)
        endif()
        add_dependencies(tdlua_native_codec_test tdlua_native_generated)
        add_test(NAME tdlua_native_codec COMMAND tdlua_native_codec_test)

        add_executable(tdlua_native_schema_test
            "tests/native_schema_test.cpp"
        )
        target_link_libraries(tdlua_native_schema_test PRIVATE tdtl)
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

    if(LUA_EXECUTABLE)
        set(TDLUA_LUA_EXECUTABLE "${LUA_EXECUTABLE}")
    else()
        find_program(TDLUA_LUA_EXECUTABLE
            NAMES "lua${TDLUA_LUA_VERSION}" lua5.5 lua5.4 lua5.3 lua5.2 lua5.1 luajit lua)
    endif()
    if(TDLUA_LUA_EXECUTABLE)
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
        tdlua_enable_sanitizer_test_runtime(
            tdlua_lifecycle
            tdlua_async
            tdlua_backend_parity
            tdlua_e2e_smoke
        )
    endif()
endif()
