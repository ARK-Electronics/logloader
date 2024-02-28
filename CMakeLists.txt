cmake_minimum_required(VERSION 3.16 FATAL_ERROR)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

project(logloader VERSION 0.1 LANGUAGES CXX)

find_package(OpenSSL REQUIRED)

string(ASCII 27 Esc)
set(White       "${Esc}[m")
set(Red         "${Esc}[31m")
set(Green       "${Esc}[32m")
set(Yellow      "${Esc}[33m")
set(Blue        "${Esc}[34m")
set(BoldGreen   "${Esc}[1;32m")

# Assumes MAVSDK system wide install
list(APPEND CMAKE_PREFIX_PATH "/usr/local/MAVSDK/install")
find_package(MAVSDK REQUIRED)

# Determine git version
exec_program(
    "git"
    ${CMAKE_CURRENT_SOURCE_DIR}
    ARGS "describe"
    OUTPUT_VARIABLE GIT_TAG
 )

#...if not found get the SHA1 hash instead
if(${GIT_TAG} MATCHES "fatal:.*")
    exec_program(
        "git"
        ${CMAKE_CURRENT_SOURCE_DIR}
        ARGS "rev-parse HEAD"
        OUTPUT_VARIABLE GIT_VERSION
    )
else()
    set(GIT_VERSION ${GIT_TAG})
endif()

add_definitions(-DAPP_GIT_VERSION="${GIT_VERSION}")

message("-- Version: ${BoldGreen}${GIT_VERSION}${White}")

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

# Installation rules
# Binary installation
install(TARGETS logloader RUNTIME DESTINATION bin)

# Directory for logs
set(INSTALL_LOG_DIR "${CMAKE_INSTALL_PREFIX}/logloader/logs")
install(DIRECTORY DESTINATION ${INSTALL_LOG_DIR})

# Config file installation and renaming
install(CODE "
    file(INSTALL ${CMAKE_SOURCE_DIR}/install.config.toml DESTINATION ${CMAKE_INSTALL_PREFIX}/logloader)
    file(RENAME ${CMAKE_INSTALL_PREFIX}/logloader/install.config.toml ${CMAKE_INSTALL_PREFIX}/logloader/config.toml)
")
