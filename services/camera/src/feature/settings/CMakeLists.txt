argus_module(NAME camera-settings
    SOURCES camera-settings.cc
    INCLUDES ../..
    DEPENDS argus::lib::config)

target_compile_options(argus_camera-settings PRIVATE -Wall -Wextra)
