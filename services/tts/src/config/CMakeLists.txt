argus_module(NAME tts-config
    SOURCES
        tts-config.cc
    INCLUDES
        ..
    DEPENDS
        argus::lib::config
        argus::lib::http
        argus::contracts::settings-wire)

target_compile_options(argus_tts-config PRIVATE -Wall -Wextra)
