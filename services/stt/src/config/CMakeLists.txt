argus_module(NAME stt-config
    SOURCES
        stt-config.cc
    INCLUDES
        ..
    DEPENDS
        argus::lib::config
        argus::lib::http)

target_compile_options(argus_stt-config PRIVATE -Wall -Wextra)
