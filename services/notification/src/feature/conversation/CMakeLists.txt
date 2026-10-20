argus_module(NAME notification-conversation
    SOURCES
        controllers/conversation-controller.cc
        dtos/conversation-thread-dto.cc
        dtos/response-conversation-threads-dto.cc
        dtos/send-message-dto.cc
        infra/call-live-announce.cc
        infra/notification-message-notice.cc
        infra/voice-message-announcer.cc
        repositories/conversation-message/conversation-message-repository.cc
        schemas/conversation-message/conversation-message-schema.cc
        services/conversation-inbox-service.cc
    INCLUDES ../..
    DEPENDS
        argus::notification-shared
        argus::notification-call
        argus::clients::voice
        argus::contracts::llm
        argus::contracts::notification
        argus::contracts::sync
        argus::lib::auth
        argus::lib::errors
        argus::lib::http
        argus::lib::runtime
        argus::lib::sqlite
        argus::lib::validation
        Drogon::Drogon)

target_compile_options(argus_notification-conversation PRIVATE -Wall -Wextra)
