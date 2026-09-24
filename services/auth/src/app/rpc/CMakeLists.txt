argus_module(NAME auth-rpc
    SOURCES
        auth-rpc-service.cc
    INCLUDES ../..
    DEPENDS
        argus::auth-session
        argus::auth-device
        argus::clients::auth
        argus::lib::grpc-health)

target_compile_options(argus_auth-rpc PRIVATE -Wall -Wextra)
