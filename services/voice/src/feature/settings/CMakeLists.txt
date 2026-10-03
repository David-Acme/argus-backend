argus_module(NAME voice-settings
    SOURCES voice-settings.cc
    INCLUDES ../..
    DEPENDS argus::lib::config)

target_compile_options(argus_voice-settings PRIVATE -Wall -Wextra)
