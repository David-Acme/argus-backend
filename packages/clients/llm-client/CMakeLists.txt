# argus_llm-client: the argus-llm wire seen from the caller's side — the HTTP
# client and the chat contract it speaks. argus-memory and argus-voice both
# stream completions through this; argus-llm links it for the contract, the
# same shape tts-client has for the TTS wire.
#
# It also owns the tool vocabulary (contracts, registry, validator, executor):
# the descriptors travel with the chat contract, every consumer of the wire
# already links this module, and the pieces used to sit in argus-memory where
# argus-llm could not reach them.

include(${CMAKE_CURRENT_SOURCE_DIR}/../../../cmake/argus-module.cmake)

if(NOT TARGET argus::auth)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../auth
                     ${CMAKE_CURRENT_BINARY_DIR}/auth)
endif()

if(NOT TARGET contract::auth)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../contracts/auth-contract
                     ${CMAKE_CURRENT_BINARY_DIR}/auth-contract)
endif()

if(NOT TARGET contract::sync)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../contracts/sync-contract
                     ${CMAKE_CURRENT_BINARY_DIR}/sync-contract)
endif()

if(NOT TARGET argus::config)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../config
                     ${CMAKE_CURRENT_BINARY_DIR}/config)
endif()

find_package(Drogon CONFIG REQUIRED)
find_package(nlohmann_json REQUIRED)

argus_module(NAME llm-client
    SOURCES
        src/shared/services/llm/remote/llm-remote.cc
        src/shared/services/tools/tool-registry.cc
        src/shared/services/tools/tool-validator.cc
        src/shared/services/tools/tool-executor.cc
    INCLUDES
        src
    DEPENDS
        argus::auth contract::auth contract::sync argus::config
        Drogon::Drogon
        nlohmann_json::nlohmann_json)

target_compile_options(argus_llm-client PRIVATE -Wall -Wextra)
