include(${CMAKE_CURRENT_SOURCE_DIR}/../../../cmake/argus-module.cmake)

if(NOT TARGET argus::lib::config)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../lib/config
                     ${CMAKE_CURRENT_BINARY_DIR}/config)
endif()

if(NOT TARGET argus::lib::errors)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../lib/errors
                     ${CMAKE_CURRENT_BINARY_DIR}/errors)
endif()

find_package(Drogon CONFIG REQUIRED)

set(CMAKE_FIND_PACKAGE_TARGETS_GLOBAL TRUE)
argus_contracts_substrate()
if(NOT ARGUS_SYSTEM_PROTOBUF AND NOT TARGET gRPC::grpc++)
    find_package(gRPC CONFIG REQUIRED)
endif()
if(NOT TARGET argus::contracts::tts)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../contracts/tts
                     ${CMAKE_BINARY_DIR}/contracts/tts)
endif()
argus_tts_rpc_contract()

argus_clients(NAME tts
    SOURCES
        src/tts/tts-client.cc
        src/tts/tts-remote.cc
    INCLUDES
        src
    DEPENDS
        argus::lib::config argus::lib::errors
        argus::contracts::tts
        Drogon::Drogon)

target_link_libraries(argus_clients_tts PRIVATE argus::contracts::tts-wire)
target_compile_options(argus_clients_tts PRIVATE -Wall -Wextra)

enable_testing()
find_package(doctest REQUIRED)

add_executable(tts-client-test tests/unit/tts-client-test.cc)

set_target_properties(tts-client-test PROPERTIES EXCLUDE_FROM_ALL FALSE)

target_link_libraries(tts-client-test PRIVATE argus::clients::tts
    doctest::doctest)

target_compile_options(tts-client-test PRIVATE -Wall -Wextra)

add_test(NAME tts-client-test COMMAND tts-client-test)
