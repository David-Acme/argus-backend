argus_module(NAME identity-sign-in
    SOURCES
        services/face-sign-in-service.cc
    INCLUDES ../..
    DEPENDS
        argus::identity-config
        argus::identity-shared
        argus::lib::runtime
        Drogon::Drogon)

target_compile_options(argus_identity-sign-in PRIVATE -Wall -Wextra)
