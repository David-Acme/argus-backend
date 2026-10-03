argus_module(NAME tts-settings
    SOURCES tts-settings.cc
    INCLUDES ../..
    DEPENDS argus::lib::config)

target_compile_options(argus_tts-settings PRIVATE -Wall -Wextra)
