argus_module(NAME camera-actions
    SOURCES
        audio-capture.cc
        camera-action-rpc-service.cc
        stt-transcriber.cc
        repositories/action-command/action-command-repository.cc
    INCLUDES
        ../..
    DEPENDS
        argus::camera-control
        argus::camera-driver
        argus::camera-privacy
        argus::camera-repositories
        argus::camera-stream
        argus::clients::camera-actions
        argus::clients::stt
        argus::lib::audio
        argus::lib::config
        argus::lib::runtime
        argus::lib::sqlite
        argus::lib::text
        Drogon::Drogon)

target_compile_options(argus_camera-actions PRIVATE -Wall -Wextra)
