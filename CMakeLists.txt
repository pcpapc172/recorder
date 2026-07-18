cmake_minimum_required(VERSION 3.21)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
project(gd-screen-recorder VERSION 1.0.0)

# Keep this project on ordinary per-source compilation.  Unity builds and
# precompiled headers can trigger MSVC CL.exe crashes during PDB/code
# generation (especially with the large Geode/FFmpeg headers).
set(CMAKE_UNITY_BUILD OFF CACHE BOOL "Disable unity builds" FORCE)
set(CMAKE_DISABLE_PRECOMPILE_HEADERS ON CACHE BOOL "Disable precompiled headers" FORCE)

file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS src/*.cpp)
add_library(${PROJECT_NAME} SHARED ${SOURCES})

if (NOT DEFINED ENV{GEODE_SDK})
    message(FATAL_ERROR "GEODE_SDK environment variable is not set")
endif()
add_subdirectory($ENV{GEODE_SDK} ${CMAKE_CURRENT_BINARY_DIR}/geode)
setup_geode_mod(${PROJECT_NAME})

# setup_geode_mod may enable PCH on the target; explicitly disable it here.
set_target_properties(${PROJECT_NAME} PROPERTIES
    UNITY_BUILD OFF
    DISABLE_PRECOMPILE_HEADERS ON
)
