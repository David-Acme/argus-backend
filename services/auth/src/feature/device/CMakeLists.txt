argus_module(NAME auth-device
    SOURCES
        schemas/device-credential/device-credential-schema.cc
        schemas/device-login-challenge/device-login-challenge-schema.cc
        repositories/device-credential/device-credential-repository.cc
        repositories/device-login-challenge/device-login-challenge-repository.cc
    INCLUDES ../..
    DEPENDS
        argus::contracts::auth
        argus::lib::sqlite
        Drogon::Drogon)

target_compile_options(argus_auth-device PRIVATE -Wall -Wextra)
