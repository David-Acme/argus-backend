argus_module(NAME vlm-rpc SOURCES vlm-rpc-server.cc INCLUDES ../..)
target_link_libraries(argus_vlm-rpc PUBLIC argus::clients::vlm
    argus::contracts::vlm-wire argus::vlm)
target_compile_options(argus_vlm-rpc PRIVATE -Wall -Wextra)
