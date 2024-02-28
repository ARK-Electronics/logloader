cmake_minimum_required(VERSION 3.16 FATAL_ERROR)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

project(logloader VERSION 0.9 LANGUAGES CXX)

find_package(OpenSSL REQUIRED)

# Assumes MAVSDK system wide install
list(APPEND CMAKE_PREFIX_PATH "/usr/local/MAVSDK/install")
find_package(MAVSDK REQUIRED)

# Add header only libraries
include_directories(third_party/cpp-httplib/)
include_directories(third_party/tomlplusplus/)

# Create executable
add_executable(logloader
    src/main.cpp
    src/LogLoader.cpp
)

target_link_libraries(logloader
    OpenSSL::SSL
    OpenSSL::Crypto
    MAVSDK::mavsdk
)

# Binary installation
install(TARGETS logloader RUNTIME DESTINATION bin)
