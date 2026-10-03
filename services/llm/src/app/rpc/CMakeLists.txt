argus_module(NAME llm-rpc SOURCES llm-rpc-server.cc INCLUDES ../..)
target_link_libraries(argus_llm-rpc PUBLIC argus::clients::llm
    argus::contracts::llm-wire argus::llm
    argus::contracts::settings-wire)
target_compile_options(argus_llm-rpc PRIVATE -Wall -Wextra)
