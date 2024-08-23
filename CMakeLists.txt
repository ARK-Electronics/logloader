cmake_minimum_required(VERSION 3.16 FATAL_ERROR)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

project(logloader VERSION 0.9 LANGUAGES CXX)

add_compile_options(-Wall -Wextra -Wpedantic -Wunused)

find_package(OpenSSL 3.0.2 REQUIRED)

message(STATUS "OpenSSL version: ${OPENSSL_VERSION}")
message(STATUS "OpenSSL include dir: ${OPENSSL_INCLUDE_DIR}")
message(STATUS "OpenSSL libraries: ${OPENSSL_LIBRARIES}")

# Assumes MAVSDK system wide install
list(APPEND CMAKE_PREFIX_PATH "/usr/local/MAVSDK/install")
find_package(MAVSDK REQUIRED)

include_directories(third_party/cpp-httplib/)
include_directories(third_party/tomlplusplus/)

add_executable(${PROJECT_NAME}
    src/main.cpp
    src/LogLoader.cpp)

target_link_libraries(${PROJECT_NAME}
    pthread
    OpenSSL::SSL
    OpenSSL::Crypto
    MAVSDK::mavsdk)
