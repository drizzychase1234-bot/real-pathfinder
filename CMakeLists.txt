cmake_minimum_required(VERSION 3.21)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)

project(RealPathfinder VERSION 1.0.0)

option(RPF_BUILD_MOD "Build the Geode mod" ON)
option(RPF_BUILD_TESTS "Build the standalone search tests" OFF)

# All source files live in the top folder of the repo.
# Game-independent search engine (unit tested on any machine).
add_library(rpf_core STATIC Search.cpp Export.cpp)
target_include_directories(rpf_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(rpf_core PROPERTIES POSITION_INDEPENDENT_CODE ON)

if (RPF_BUILD_TESTS)
    enable_testing()
    add_executable(rpf_tests SearchTests.cpp)
    target_link_libraries(rpf_tests PRIVATE rpf_core)
    add_test(NAME search COMMAND rpf_tests)
endif()

if (RPF_BUILD_MOD)
    if (NOT DEFINED ENV{GEODE_SDK})
        message(FATAL_ERROR "Unable to find Geode SDK! Please define GEODE_SDK environment variable to point to Geode")
    endif()
    add_library(${PROJECT_NAME} SHARED GdGame.cpp Hooks.cpp UI.cpp)
    target_include_directories(${PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    add_subdirectory($ENV{GEODE_SDK} ${CMAKE_CURRENT_BINARY_DIR}/geode)
    target_link_libraries(${PROJECT_NAME} PRIVATE rpf_core)
    setup_geode_mod(${PROJECT_NAME})
endif()
