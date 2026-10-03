argus_module(NAME vlm-config
    SOURCES
        vlm-config.cc
    INCLUDES
        ..
    DEPENDS
        argus::lib::config
        argus::lib::http
        argus::contracts::settings-wire)

target_compile_options(argus_vlm-config PRIVATE -Wall -Wextra)
