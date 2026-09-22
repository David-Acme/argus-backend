argus_module(NAME sync-config
    SOURCES
        sync-config.cc
    INCLUDES ..
    DEPENDS
        argus::lib::config
        argus::lib::http)

target_compile_options(argus_sync-config PRIVATE -Wall -Wextra)
