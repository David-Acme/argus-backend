argus_module(NAME conversation
    SOURCES
        repositories/conversation/conversation-repository.cc
        services/switch-note.cc
        services/conversation-service.cc
        services/conversation-context.cc
        services/conversation-purge.cc
        services/conversation-transcript.cc
        infra/notification-conversation-transcript.cc
    INCLUDES
        ../..
    DEPENDS
        argus::contracts::llm
        argus::clients::notification
        argus::lib::runtime
        argus::lib::sqlite
        Drogon::Drogon)

target_compile_options(argus_conversation PRIVATE -Wall -Wextra)
