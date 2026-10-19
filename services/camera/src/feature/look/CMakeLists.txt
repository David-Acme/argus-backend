argus_module(NAME camera-look
    SOURCES
        services/follow-service.cc
        services/look-refresh.cc
        services/look-service.cc
        services/second-look.cc
    INCLUDES
        ../..
    DEPENDS
        argus::camera-config
        argus::camera-control
        argus::camera-driver
        argus::camera-objects
        argus::camera-privacy-state
        argus::camera-recording
        argus::camera-repositories
        argus::camera-stream
        argus::lib::runtime
        argus::lib::text
        opencv::opencv
        Drogon::Drogon)

target_compile_options(argus_camera-look PRIVATE -Wall -Wextra)
