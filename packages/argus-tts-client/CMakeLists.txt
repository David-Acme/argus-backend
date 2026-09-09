# argus_tts-client: the argus-tts wire seen from the caller's side — the wire
# contract itself (tts-wire.hxx) and the HTTP client that speaks it. Both
# argus-camera (the Tapo talk path) and argus-voice synthesize through this,
# and argus-tts links it for the contract, the same shape argus-contracts has
# for the gRPC services.

include(${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/argus-module.cmake)

find_package(Drogon CONFIG REQUIRED)

argus_module(NAME tts-client
    SOURCES
        src/shared/services/tts/remote/tts-remote.cc
    INCLUDES
        src
    DEPENDS
        argus_common
        Drogon::Drogon)

target_compile_options(argus_tts-client PRIVATE -Wall -Wextra)
