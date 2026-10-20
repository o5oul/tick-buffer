cmake_minimum_required(VERSION 3.21)

# Set up the project configuration variables
project(TickBuffer VERSION 1.0.0)

# Inject the environment paths so find_package knows where to look
if(DEFINED ENV{GEODE_SDK})
    list(APPEND CMAKE_PREFIX_PATH "$ENV{GEODE_SDK}")
endif()

# Locate the Geode SDK framework (Forces the stable v4 setup)
find_package(Geode REQUIRED v4.0.0)

# Set up the compiler features for modern Geode development
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Locate and include your source files
# (Note: Change this to main.cpp if you moved it directly out of the src folder!)
add_library(${PROJECT_NAME} SHARED src/main.cpp)

# Link everything together via the Geode build chain
setup_geode_mod(${PROJECT_NAME})
