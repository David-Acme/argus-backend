argus_module(NAME tts-rpc SOURCES tts-rpc-server.cc INCLUDES ../..)
target_link_libraries(argus_tts-rpc PUBLIC argus::clients::tts
    argus::contracts::tts-wire argus::tts-synthesis)
target_compile_options(argus_tts-rpc PRIVATE -Wall -Wextra)
