cmake_minimum_required(VERSION 3.25)
project(argus-cert VERSION 1.0.0 LANGUAGES CXX)

enable_testing()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(${CMAKE_CURRENT_SOURCE_DIR}/../../../cmake/argus-module.cmake)

find_package(Drogon CONFIG REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(OpenSSL REQUIRED)

if(NOT TARGET argus::lib::config)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../config
                     ${CMAKE_BINARY_DIR}/config)
endif()

argus_lib(NAME cert
    SOURCES
        src/cert/cert-service.cc
    INCLUDES src)

target_link_libraries(argus_lib_cert PUBLIC
    argus::lib::config
    Drogon::Drogon
    nlohmann_json::nlohmann_json
    OpenSSL::SSL
    OpenSSL::Crypto)

target_compile_options(argus_lib_cert PRIVATE -Wall -Wextra)

find_package(doctest REQUIRED)

add_executable(cert-san-test tests/unit/cert-san-test.cc)
target_link_libraries(cert-san-test PRIVATE
    argus::lib::cert
    argus::lib::config
    doctest::doctest)
target_compile_options(cert-san-test PRIVATE -Wall -Wextra)
add_test(NAME cert-san-test COMMAND cert-san-test)
