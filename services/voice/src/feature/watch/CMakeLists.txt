argus_module(NAME voice-watch
    SOURCES
        camera-ear.cc
        camera-level.cc
        echo-guard.cc
        fire-log.cc
        stage-two.cc
        wake-spotter.cc
        wake-words.cc
        watch-coordinator.cc
    INCLUDES
        ../..
    DEPENDS
        argus::voice-core
        argus::voice-config
        argus::clients::camera-watch
        argus::clients::stt
        argus::contracts::voice
        argus::lib::config
        Drogon::Drogon)

target_compile_options(argus_voice-watch PRIVATE -Wall -Wextra)
