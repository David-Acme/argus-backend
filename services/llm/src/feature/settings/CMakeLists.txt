argus_module(NAME llm-settings
    SOURCES llm-settings.cc llm-components.cc
    INCLUDES ../..
    DEPENDS argus::lib::config argus::contracts::settings-wire)

target_compile_options(argus_llm-settings PRIVATE -Wall -Wextra)
