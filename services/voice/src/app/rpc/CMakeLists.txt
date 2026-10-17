argus_module(NAME voice-rpc
    SOURCES
        voice-rpc-server.cc
    INCLUDES
        ../..
    DEPENDS
        argus::lib::grpc-health)

target_compile_options(argus_voice-rpc PRIVATE -Wall -Wextra)
