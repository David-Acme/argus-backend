argus_module(NAME sync-rtc
    SOURCES
        controllers/rtc-controller.cc
        dtos/rtc-token-dto.cc
        dtos/response-rtc-token-dto.cc
        services/livekit-token.cc
        services/rtc-naming.cc
        services/rtc-token-service.cc
        services/rtc-session-revoker.cc
        infra/livekit-room-client.cc
        infra/voice-room-joiner.cc
        infra/notification-call-claimer.cc
    INCLUDES ../..
    DEPENDS
        argus::sync-config
        argus::clients::notification
        argus::clients::voice
        argus::contracts::auth
        argus::lib::auth
        argus::lib::errors
        argus::lib::http
        argus::lib::runtime
        argus::lib::text
        argus::lib::validation
        Drogon::Drogon
    SYSTEM_DEPENDS
        jwt-cpp::jwt-cpp
        nlohmann_json::nlohmann_json)

target_compile_options(argus_sync-rtc PRIVATE -Wall -Wextra)
