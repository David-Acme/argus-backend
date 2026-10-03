argus_module(NAME llm-settings
    SOURCES llm-settings.cc
    INCLUDES ../..
    DEPENDS argus::lib::config)

target_compile_options(argus_llm-settings PRIVATE -Wall -Wextra)
