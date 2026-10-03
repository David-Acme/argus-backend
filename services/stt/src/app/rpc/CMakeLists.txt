argus_module(NAME stt-rpc SOURCES stt-rpc-server.cc INCLUDES ../..)
target_link_libraries(argus_stt-rpc PUBLIC argus::clients::stt
    argus::contracts::stt-wire argus::stt
    argus::contracts::settings-wire argus::stt-settings)
target_compile_options(argus_stt-rpc PRIVATE -Wall -Wextra)
