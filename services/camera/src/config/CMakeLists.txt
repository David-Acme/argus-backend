argus_module(NAME camera-config
    SOURCES
        camera-config.cc
        operator-config.cc
    INCLUDES
        ..
    DEPENDS
        argus::lib::config
        argus::lib::http
        argus::lib::text
        argus::contracts::settings-wire
        Drogon::Drogon)

target_compile_options(argus_camera-config PRIVATE -Wall -Wextra)
