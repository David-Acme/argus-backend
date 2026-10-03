argus_module(NAME tts-provisioning
    SOURCES
        infra/pocket-catalog.cc
        infra/provision-process.cc
        services/pocket-choice-states.cc
        services/pocket-installer.cc
        services/pocket-provisioning.cc
    INCLUDES ../..
    DEPENDS
        argus::tts-synthesis
        argus::lib::config
        Drogon::Drogon)

target_compile_options(argus_tts-provisioning PRIVATE -Wall -Wextra)
