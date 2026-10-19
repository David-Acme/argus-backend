argus_module(NAME voice-watch
    SOURCES
        camera-conversation.cc
        camera-ear.cc
        camera-gains.cc
        camera-identity.cc
        camera-level.cc
        conversation-policy.cc
        echo-guard.cc
        fire-log.cc
        room-directory.cc
        speaker-resolution.cc
        stage-two.cc
        wake-spotter.cc
        watch-directory.cc
        watch-sync.cc
        wake-words.cc
        watch-conversations.cc
        watch-coordinator.cc
    INCLUDES
        ../..
    DEPENDS
        argus::voice-core
        argus::voice-config
        argus::clients::camera-watch
        argus::clients::guard
        argus::clients::identity
        argus::clients::notification
        argus::clients::sync
        argus::clients::stt
        argus::contracts::voice
        argus::lib::audio
        argus::lib::config
        Drogon::Drogon)

target_compile_options(argus_voice-watch PRIVATE -Wall -Wextra)
