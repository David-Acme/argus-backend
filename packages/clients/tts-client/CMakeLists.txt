# tts-client: the argus-tts wire seen from the caller's side — the modern
# gRPC client plus the adaptive remote client that speaks it, with the
# transitional HTTP fallback. Both argus-camera (the Tapo talk path) and
# argus-voice synthesize through this, and argus-tts links it for the
# contract, the same shape argus-contracts has for the gRPC services.

include(${CMAKE_CURRENT_SOURCE_DIR}/../../../cmake/argus-module.cmake)

if(NOT TARGET argus::config)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../config
                     ${CMAKE_CURRENT_BINARY_DIR}/config)
endif()

if(NOT TARGET pkg::response)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../response
                     ${CMAKE_CURRENT_BINARY_DIR}/response)
endif()

find_package(Drogon CONFIG REQUIRED)

set(CMAKE_FIND_PACKAGE_TARGETS_GLOBAL TRUE)
argus_contracts_substrate()
if(NOT ARGUS_SYSTEM_PROTOBUF AND NOT TARGET gRPC::grpc++)
    find_package(gRPC CONFIG REQUIRED)
endif()
if(NOT TARGET contract::tts)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../contracts/tts-contract
                     ${CMAKE_CURRENT_BINARY_DIR}/tts-contract)
endif()
argus_tts_rpc_contract()

argus_module(NAME tts-client
    SOURCES
        tts-client.cc
        src/shared/services/tts/remote/tts-remote.cc
    INCLUDES
        .
        src
    DEPENDS
        argus::config pkg::response
        contract::tts
        Drogon::Drogon)

target_link_libraries(argus_tts-client PRIVATE argus::client-tts-wire)
target_compile_options(argus_tts-client PRIVATE -Wall -Wextra)
