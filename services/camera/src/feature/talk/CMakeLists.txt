argus_module(NAME camera-talk
    SOURCES
        camera-talk-service.cc
        camera-talk-session.cc
        talk-uplink.cc
    INCLUDES
        ../..
    DEPENDS
        argus::camera-driver
        argus::camera-repositories
        argus::contracts::camera
        argus::contracts::sync
        argus::lib::audio
        argus::lib::auth
        argus::lib::errors
        Drogon::Drogon)

target_compile_options(argus_camera-talk PRIVATE -Wall -Wextra)
