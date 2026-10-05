argus_module(NAME identity-rate-gate
    SOURCES
        infra/identity-rate-gate.cc
        infra/rate-limiter.cc
    INCLUDES ../..
    DEPENDS
        argus::identity-config
        argus::lib::auth
        argus::lib::errors
        argus::lib::http
        argus::contracts::identity
        Drogon::Drogon)

target_compile_options(argus_identity-rate-gate PRIVATE -Wall -Wextra)
