argus_module(NAME tts-settings
    SOURCES tts-settings.cc tts-components.cc
    INCLUDES ../..
    DEPENDS argus::lib::config argus::contracts::settings-wire)

target_compile_options(argus_tts-settings PRIVATE -Wall -Wextra)
