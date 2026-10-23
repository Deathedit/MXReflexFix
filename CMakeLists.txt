cmake_minimum_required(VERSION 3.24)
project(MXReflexFix VERSION 0.1.0 LANGUAGES C CXX)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "MXReflex.exe is 32-bit: configure with -DCMAKE_TOOLCHAIN_FILE=cmake/mingw32.cmake")
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE OFF)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

include(FetchContent)

set(SAFETYHOOK_FETCH_ZYDIS ON CACHE BOOL "" FORCE)
FetchContent_Declare(safetyhook
    GIT_REPOSITORY https://github.com/cursey/safetyhook.git
    GIT_TAG        v0.6.10
    GIT_SHALLOW    TRUE)

set(SPDLOG_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG        v1.15.3
    GIT_SHALLOW    TRUE)

# Header-only; SOURCE_SUBDIR points nowhere so its own CMake (tests) is not added.
FetchContent_Declare(inipp
    GIT_REPOSITORY https://github.com/mcmtroffaes/inipp.git
    GIT_TAG        develop
    GIT_SHALLOW    TRUE
    SOURCE_SUBDIR  _none)

FetchContent_MakeAvailable(safetyhook spdlog inipp)

set(FIX_VERSION_STRING "${PROJECT_VERSION}" CACHE STRING "Version shown in MXReflexFix.log")

add_library(MXReflexFix SHARED
    src/dllmain.cpp)

set_target_properties(MXReflexFix PROPERTIES
    PREFIX ""
    SUFFIX ".asi")

target_include_directories(MXReflexFix PRIVATE
    src
    ${inipp_SOURCE_DIR}/inipp)

target_compile_definitions(MXReflexFix PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _WIN32_WINNT=0x0601
    FIX_VERSION="${FIX_VERSION_STRING}")

target_link_libraries(MXReflexFix PRIVATE
    safetyhook::safetyhook
    spdlog::spdlog)

# Fold libgcc/libstdc++/thread runtime into the DLL so nothing extra ships next to the game.
target_link_options(MXReflexFix PRIVATE
    -static
    -static-libgcc
    -static-libstdc++
    $<$<CONFIG:Release>:-s>)

# Print the DLL imports: only Windows system DLLs may appear here.
add_custom_command(TARGET MXReflexFix POST_BUILD
    COMMAND sh -c "${CMAKE_OBJDUMP} -p '$<TARGET_FILE:MXReflexFix>' | grep 'DLL Name' || true"
    COMMENT "MXReflexFix.asi imports (must be system DLLs only):"
    VERBATIM)
