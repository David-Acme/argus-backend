argus_module(NAME messenger
    SOURCES
        services/relay-command.cc
        services/relay-recipient.cc
        repositories/relay-thread/relay-thread-repository.cc
        repositories/visitor-notice/visitor-notice-repository.cc
        services/relay-lines.cc
        services/relay-service.cc
        services/relay-tool.cc
        services/messenger-wiring.cc
        infra/notification-message-courier.cc
        infra/conversation-inbound.cc
        infra/identity-household-roster.cc
        infra/graph-messenger-store.cc
    INCLUDES
        ../..
    DEPENDS
        argus::lib::errors
        argus::clients::notification
        argus::clients::identity
        argus::memory
        argus::conversation
        argus::contracts::auth
        argus::lib::mcp
        argus::llm-shared
        argus::lib::sqlite
        argus::lib::text
        Drogon::Drogon)

target_compile_options(argus_messenger PRIVATE -Wall -Wextra)
