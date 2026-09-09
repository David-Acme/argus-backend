# argus_stt-client: the argus-stt wire seen from the caller's side. The header
# IS the contract (16 kHz mono s16 PCM in, transcript out), so argus-stt
# includes it as well — the same shape argus-tts-client and argus-llm-client
# have for their wires.

include(${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/argus-module.cmake)

find_package(Drogon CONFIG REQUIRED)

argus_module(NAME stt-client
    SOURCES
        src/shared/services/stt/remote/stt-remote.cc
    INCLUDES
        src
    DEPENDS
        argus_common
        Drogon::Drogon)

target_compile_options(argus_stt-client PRIVATE -Wall -Wextra)
