argus_module(NAME auth-config
    SOURCES
        auth-config.cc
    INCLUDES ..
    DEPENDS
        argus::lib::config
        argus::lib::grpc-health
        argus::lib::http)

target_compile_options(argus_auth-config PRIVATE -Wall -Wextra)
