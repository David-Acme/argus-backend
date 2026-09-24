argus_module(NAME auth-device
    SOURCES
        schemas/device-credential/device-credential-schema.cc
        repositories/device-credential/device-credential-repository.cc
    INCLUDES ../..
    DEPENDS
        argus::lib::sqlite
        Drogon::Drogon)

target_compile_options(argus_auth-device PRIVATE -Wall -Wextra)
