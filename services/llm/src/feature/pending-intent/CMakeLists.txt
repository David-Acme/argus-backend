argus_module(NAME pending-intent
    SOURCES
        infra/notification-intent-notifier.cc
        repositories/pending-intent/pending-intent-repository.cc
        services/module-intent-feed.cc
        services/pending-intent-service.cc
    INCLUDES
        ../..
    DEPENDS
        argus::llm-shared
        argus::lib::nats
        argus::lib::sqlite
        argus::lib::text
        argus::clients::notification
        argus::memory
        Drogon::Drogon)

target_compile_options(argus_pending-intent PRIVATE -Wall -Wextra)
