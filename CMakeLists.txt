cmake_minimum_required(VERSION 3.16 FATAL_ERROR)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

project(logloader VERSION 1.0 LANGUAGES CXX)

# Without this the default is an empty build type, i.e. no optimisation at all.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()
message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")

add_compile_options(-Wall -Wextra -Werror -Wpedantic -Wunused)

find_package(OpenSSL 3.0.2 REQUIRED)
find_package(SQLite3 REQUIRED)
find_package(Threads REQUIRED)

message(STATUS "OpenSSL version: ${OPENSSL_VERSION}")
message(STATUS "SQLite3 version: ${SQLite3_VERSION}")

# Assumes a system-wide MAVSDK install
list(APPEND CMAKE_PREFIX_PATH "/usr/local/MAVSDK/install")
find_package(MAVSDK REQUIRED)

add_executable(${PROJECT_NAME}
    src/main.cpp
    src/ApiServer.cpp
    src/Config.cpp
    src/FtpListClient.cpp
    src/FtpLogFetcher.cpp
    src/Log.cpp
    src/LogDatabase.cpp
    src/LogLoader.cpp
    src/Sqlite.cpp
    src/StatusBoard.cpp
    src/UploadTarget.cpp)

# Must be set for every translation unit that sees httplib.h: it changes the
# layout of httplib's socket types, so a per-file #define is an ODR violation.
target_compile_definitions(${PROJECT_NAME} PRIVATE CPPHTTPLIB_OPENSSL_SUPPORT)

# The vendored headers are SYSTEM so their warnings are not ours to fix.
target_include_directories(${PROJECT_NAME} SYSTEM PRIVATE
    third_party/cpp-httplib
    third_party/tomlplusplus
    third_party/json/single_include
    ${SQLite3_INCLUDE_DIRS})

target_link_libraries(${PROJECT_NAME}
    Threads::Threads
    OpenSSL::SSL
    OpenSSL::Crypto
    MAVSDK::mavsdk
    ${SQLite3_LIBRARIES})
