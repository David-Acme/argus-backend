argus_module(NAME stt-settings
    SOURCES stt-settings.cc stt-components.cc
    INCLUDES ../..
    DEPENDS argus::lib::config argus::contracts::settings-wire)

target_compile_options(argus_stt-settings PRIVATE -Wall -Wextra)
