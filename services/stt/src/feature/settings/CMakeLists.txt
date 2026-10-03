argus_module(NAME stt-settings
    SOURCES stt-settings.cc
    INCLUDES ../..
    DEPENDS argus::lib::config)

target_compile_options(argus_stt-settings PRIVATE -Wall -Wextra)
