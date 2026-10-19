argus_module(NAME camera-watch
    SOURCES
        camera-watch-rpc-service.cc
        infra/ffmpeg-microphone-source.cc
        services/microphone-hub.cc
        services/speaker-program.cc
        services/speaker-session.cc
        services/watch-camera-board.cc
        services/watch-gate.cc
    INCLUDES
        ../..
    DEPENDS
        argus::camera-config
        argus::camera-driver
        argus::camera-repositories
        argus::camera-stream
        argus::camera-talk
        argus::clients::camera-watch
        argus::contracts::camera
        Drogon::Drogon)

target_compile_options(argus_camera-watch PRIVATE -Wall -Wextra)
