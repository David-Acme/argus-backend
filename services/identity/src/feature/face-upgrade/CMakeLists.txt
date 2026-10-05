argus_module(NAME identity-face-upgrade
    SOURCES
        repositories/face-upgrade/face-upgrade-repository.cc
        services/face-upgrade-service.cc
    INCLUDES ../..
    DEPENDS
        argus::identity-shared
        argus::lib::runtime
        argus::lib::sqlite
        Drogon::Drogon)

target_compile_options(argus_identity-face-upgrade PRIVATE -Wall -Wextra)
