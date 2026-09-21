# argus_clients_tts: the argus-tts wire seen from the caller's side — the
# modern gRPC client plus the adaptive remote client that speaks it, with the
# transitional HTTP fallback. Both argus-camera (the Tapo talk path) and
# argus-voice synthesize through this, and argus-tts links it for the
# contract, the shape `packages/contracts` gives the gRPC services.

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
        tts-client.cc
        src/shared/services/tts/remote/tts-remote.cc
    INCLUDES
        .
        src
    DEPENDS
        argus::lib::config argus::lib::errors
        argus::contracts::tts
        Drogon::Drogon)

target_link_libraries(argus_clients_tts PRIVATE argus::contracts::tts-wire)
target_compile_options(argus_clients_tts PRIVATE -Wall -Wextra)
