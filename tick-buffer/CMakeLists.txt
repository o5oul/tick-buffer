cmake_minimum_required(VERSION 3.21)

# Project Properties
project(TickBuffer VERSION 1.0.0)

# Compiler Rules
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS ON)

# Source File Targets
# Note: If your main.cpp is sitting inside a 'src/' folder, keep it as src/main.cpp.
# If main.cpp is directly in the folder alongside CMakeLists.txt, change it to main.cpp.
add_library(${PROJECT_NAME} SHARED src/main.cpp)

# Ensure Environment Paths Exist
if (NOT DEFINED ENV{GEODE_SDK})
    message(FATAL_ERROR "Unable to find Geode SDK! Please define GEODE_SDK environment variable.")
endif()

# Build SDK Modules
add_subdirectory($ENV{GEODE_SDK} ${CMAKE_CURRENT_BINARY_DIR}/geode)
setup_geode_mod(${PROJECT_NAME})
