argus_module(NAME voice-config
    SOURCES
        voice-config.cc
    INCLUDES
        ..
    DEPENDS
        argus::lib::http)

target_compile_options(argus_voice-config PRIVATE -Wall -Wextra)
