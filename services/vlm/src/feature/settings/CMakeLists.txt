argus_module(NAME vlm-settings
    SOURCES vlm-settings.cc
    INCLUDES ../..
    DEPENDS argus::lib::config)

target_compile_options(argus_vlm-settings PRIVATE -Wall -Wextra)
