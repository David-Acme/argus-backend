argus_module(NAME tts-rpc SOURCES tts-rpc-server.cc INCLUDES ../..)
target_link_libraries(argus_tts-rpc PUBLIC argus::clients::tts
    argus::contracts::tts-wire argus::tts-synthesis
    argus::contracts::settings-wire argus::tts-settings)
target_compile_options(argus_tts-rpc PRIVATE -Wall -Wextra)
