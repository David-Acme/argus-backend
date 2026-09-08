# argus_llm-client: the argus-llm wire seen from the caller's side — the HTTP
# client and the chat contract it speaks. argus-memory and argus-voice both
# stream completions through this; argus-llm links it for the contract, the
# same shape argus-tts-client has for the TTS wire.

include(${CMAKE_CURRENT_SOURCE_DIR}/../cmake/argus-module.cmake)

find_package(Drogon CONFIG REQUIRED)
find_package(nlohmann_json REQUIRED)

argus_module(NAME llm-client
    SOURCES
        src/shared/services/llm/remote/llm-remote.cc
    INCLUDES
        src
    DEPENDS
        argus_common
        Drogon::Drogon
        nlohmann_json::nlohmann_json)

target_compile_options(argus_llm-client PRIVATE -Wall -Wextra)
