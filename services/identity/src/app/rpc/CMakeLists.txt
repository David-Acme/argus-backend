argus_module(NAME identity-rpc
    SOURCES
        identity-rpc-service.cc
        identity-sync-rpc-service.cc
        identity-voiceprint-rpc-service.cc
    INCLUDES ../..
    DEPENDS
        argus::identity-shared
        argus::identity-enrollment
        argus::identity-person
        argus::identity-sign-in
        argus::identity-user
        argus::identity-voiceprint
        argus::identity-visitor
        argus::lib::auth
        argus::lib::errors
        argus::lib::grpc-health
        argus::lib::nats
        argus::lib::runtime
        argus::lib::sqlite
        argus::contracts::auth
        argus::contracts::identity
        argus::contracts::sync
        argus::clients::identity
        argus::clients::auth
        Drogon::Drogon)

target_compile_options(argus_identity-rpc PRIVATE -Wall -Wextra)
