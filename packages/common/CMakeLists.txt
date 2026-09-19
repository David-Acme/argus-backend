cmake_minimum_required(VERSION 3.25)
project(argus-common VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# argus_common: shared foundation library consumed by the backend, the unit
# tests, the labs and the argus-gateway service. Only transition shims stay
# here (response/exception include redirects); every module moved out to its
# own package (access, validation, json, hash, text, threading, config,
# storage, nats, sqlite helpers, hardware, sync contracts and domain seams).
# Deleting this package is the last step once no consumer links it.

include(${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/argus-module.cmake)

# s3-signing/s3-storage-service need OpenSSL; found here instead of relying
# on the caller, since not every root/standalone build path finds it before
# add_subdirectory(argus-common).
if(NOT TARGET OpenSSL::SSL)
    find_package(OpenSSL REQUIRED)
endif()

if(NOT TARGET Drogon::Drogon)
    find_package(Drogon CONFIG REQUIRED)
endif()

if(NOT TARGET cnats::nats_static)
    find_package(cnats REQUIRED)
endif()

if(NOT TARGET pkg::response)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../response
                     ${CMAKE_CURRENT_BINARY_DIR}/response)
endif()

if(NOT TARGET pkg::response-http)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../response/src/http
                     ${CMAKE_CURRENT_BINARY_DIR}/response-http)
endif()

foreach(LEAF IN ITEMS access validation json hash text threading config storage nats)
    if(NOT TARGET argus::${LEAF})
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../${LEAF}
                         ${CMAKE_CURRENT_BINARY_DIR}/${LEAF})
    endif()
endforeach()

argus_module(NAME common
    SOURCES
        src/shared/exceptions/response-exception.hxx
        src/shared/wrapper/api-response/api-response.hxx
    INCLUDES src
    DEPENDS
        argus::access
        argus::validation
        argus::json
        argus::hash
        argus::text
        argus::threading
        argus::config
        argus::storage
        argus::nats)

# Header-only since the leaf moves: no compiled source sets the linker
# language on its own.
set_target_properties(argus_common PROPERTIES LINKER_LANGUAGE CXX)

target_link_libraries(argus_common
    PUBLIC
        pkg::response-http
        Drogon::Drogon
        cnats::nats_static
    PRIVATE
        OpenSSL::SSL
        OpenSSL::Crypto
)

target_compile_options(argus_common PRIVATE -Wall -Wextra)

# The suite whose code still lives here: the envelope. Leaf suites ride their
# own packages (aggregated into this build above).
enable_testing()
find_package(doctest REQUIRED)

add_executable(api-response-test tests/unit/api-response-test.cc)
target_link_libraries(api-response-test PRIVATE argus_common doctest::doctest)
target_compile_options(api-response-test PRIVATE -Wall -Wextra)
add_test(NAME api-response-test COMMAND api-response-test)

if(NOT TARGET contract::tts)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../contracts/tts-contract
                     ${CMAKE_CURRENT_BINARY_DIR}/tts-contract)
endif()
target_link_libraries(api-response-test PRIVATE contract::tts)

get_target_property(DOCTEST_INCLUDE_DIRS doctest::doctest
                    INTERFACE_INCLUDE_DIRECTORIES)
if(DOCTEST_INCLUDE_DIRS)
    set_target_properties(doctest::doctest PROPERTIES
        INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${DOCTEST_INCLUDE_DIRS}")
endif()
