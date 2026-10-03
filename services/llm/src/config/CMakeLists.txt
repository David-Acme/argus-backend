argus_module(NAME llm-config
    SOURCES
        llm-config.cc
    INCLUDES
        ..
    DEPENDS
        argus::lib::config
        argus::lib::http
        argus::contracts::settings-wire)

target_compile_options(argus_llm-config PRIVATE -Wall -Wextra)
