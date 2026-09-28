cmake_minimum_required(VERSION 3.21)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)

project(RealPathfinder VERSION 1.0.0)

option(RPF_BUILD_MOD "Build the Geode mod" ON)
option(RPF_BUILD_TESTS "Build the standalone search tests" OFF)

# All source files live in the top folder of the repo.
if (RPF_BUILD_TESTS)
    enable_testing()
    add_executable(rpf_tests SearchTests.cpp Search.cpp Export.cpp)
    target_include_directories(rpf_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME search COMMAND rpf_tests)
endif()

if (RPF_BUILD_MOD)
    if (NOT DEFINED ENV{GEODE_SDK})
        message(FATAL_ERROR "Unable to find Geode SDK! Please define GEODE_SDK environment variable to point to Geode")
    endif()
    # The search engine is compiled straight into the mod (no extra library to link).
    add_library(${PROJECT_NAME} SHARED GdGame.cpp Hooks.cpp UI.cpp Search.cpp Export.cpp)
    target_include_directories(${PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    add_subdirectory($ENV{GEODE_SDK} ${CMAKE_CURRENT_BINARY_DIR}/geode)
    setup_geode_mod(${PROJECT_NAME})
endif()
