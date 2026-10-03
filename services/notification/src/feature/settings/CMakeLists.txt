argus_module(NAME notification-settings
    SOURCES notification-settings.cc
    INCLUDES ../..
    DEPENDS argus::lib::config)

target_compile_options(argus_notification-settings PRIVATE -Wall -Wextra)
