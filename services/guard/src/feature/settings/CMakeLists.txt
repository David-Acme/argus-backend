argus_module(NAME guard-settings
    SOURCES guard-settings.cc
    INCLUDES ../..
    DEPENDS argus::lib::config)

target_compile_options(argus_guard-settings PRIVATE -Wall -Wextra)
