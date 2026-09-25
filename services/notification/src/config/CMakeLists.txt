argus_module(NAME notification-config
    SOURCES
        notification-config.cc
    INCLUDES
        ..
    DEPENDS
        argus::lib::config
        argus::lib::http)

target_compile_options(argus_notification-config PRIVATE -Wall -Wextra)
