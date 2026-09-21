cmake_minimum_required(VERSION 3.25)
project(argus-cert VERSION 1.0.0 LANGUAGES CXX)

# enable_testing() must precede the first add_subdirectory: the flag
# propagates into every subtree, and a directory that misses it gets no
# CTestTestfile — the tests of anything added beneath it then never run.
enable_testing()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# argus_lib_cert: TLS certificate issuance for the instance's HTTPS listener.
# Consumers link argus::lib::cert.

include(${CMAKE_CURRENT_SOURCE_DIR}/../../../cmake/argus-module.cmake)

# The leaves this module links: a DAG, so their guards carry them into every
# build, standalone or consumed. The cert suite stands its own Drogon listener
# up and links nothing above tier 1, so this package's standalone configure is
# this one sibling.
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

# CertService rotation and the in-process TLS listener that proves the hot
# reload: Drogon's own listener over the rotated pair, so the suite reads the
# cert package and the TOML reader and nothing else.
find_package(doctest REQUIRED)

add_executable(cert-san-test tests/unit/cert-san-test.cc)
target_link_libraries(cert-san-test PRIVATE
    argus::lib::cert
    argus::lib::config
    doctest::doctest)
target_compile_options(cert-san-test PRIVATE -Wall -Wextra)
add_test(NAME cert-san-test COMMAND cert-san-test)
