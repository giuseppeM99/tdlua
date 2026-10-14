cmake_minimum_required(VERSION 3.20)
project(tdlua_macos_residency_poc LANGUAGES CXX)

if(NOT APPLE)
    message(FATAL_ERROR "This loader PoC must be configured and run on macOS")
endif()

include(CTest)
find_package(Threads REQUIRED)

add_library(poc_dependency SHARED dependency.cpp)
target_compile_features(poc_dependency PRIVATE cxx_std_11)
set_target_properties(poc_dependency PROPERTIES
    MACOSX_RPATH TRUE
    INSTALL_NAME_DIR "@rpath")

add_library(tdlua_poc MODULE module.cpp)
target_compile_features(tdlua_poc PRIVATE cxx_std_11)
target_link_libraries(tdlua_poc PRIVATE poc_dependency)
set_target_properties(tdlua_poc PROPERTIES
    PREFIX ""
    SUFFIX ".dylib"
    BUILD_RPATH "@loader_path"
    INSTALL_RPATH "@loader_path")

add_executable(poc_host host.cpp)
target_link_libraries(poc_host PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
target_compile_features(poc_host PRIVATE cxx_std_17)

add_test(NAME macos_loader_residency_pinned
    COMMAND poc_host --pinned $<TARGET_FILE:tdlua_poc> $<TARGET_FILE:poc_dependency>)
add_test(NAME macos_loader_residency_unpinned
    COMMAND poc_host --unpinned $<TARGET_FILE:tdlua_poc> $<TARGET_FILE:poc_dependency>)
set_tests_properties(
    macos_loader_residency_pinned
    macos_loader_residency_unpinned
    PROPERTIES TIMEOUT 15)
