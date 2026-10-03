argus_module(NAME settings-config
    SOURCES
        settings-config.cc
    INCLUDES
        ..
    DEPENDS
        argus::lib::config
        argus::lib::http
        Drogon::Drogon)

target_compile_options(argus_settings-config PRIVATE -Wall -Wextra)
