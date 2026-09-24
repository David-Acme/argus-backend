argus_module(NAME identity-config
    SOURCES
        identity-config.cc
    INCLUDES ..
    DEPENDS
        argus::lib::config
        argus::lib::http)

target_compile_options(argus_identity-config PRIVATE -Wall -Wextra)
