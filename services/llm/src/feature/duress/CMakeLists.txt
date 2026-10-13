argus_module(NAME duress
    SOURCES
        infra/guard-duress-link.cc
        infra/sqlite-duress-outbox.cc
        repositories/duress-raise/duress-raise-repository.cc
        services/duress-feed.cc
        services/duress-screen.cc
    INCLUDES
        ../..
    DEPENDS
        argus::llm-shared
        argus::clients::guard
        argus::contracts::auth
        argus::contracts::llm
        argus::contracts::sync
        argus::lib::sqlite
        argus::lib::text
        Drogon::Drogon)

target_compile_options(argus_duress PRIVATE -Wall -Wextra)
