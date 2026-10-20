cmake_minimum_required(VERSION 3.21)

# Set up the project
project(TickBuffer VERSION 1.0.0)

# Set up the compiler features for modern Geode development
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)

# Locate the Geode SDK framework (Forces the stable v4 setup)
find_package(Geode REQUIRED v4.0.0)

# Locate and include your source files
# This points directly to main.cpp if you left it in the folder, 
# or change it back to src/main.cpp if you moved it into a src subfolder!
add_library(${PROJECT_NAME} SHARED src/main.cpp)

# Link everything together via the Geode build chain
setup_geode_mod(${PROJECT_NAME})
