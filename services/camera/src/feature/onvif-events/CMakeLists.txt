argus_module(NAME camera-onvif-events
    SOURCES
        onvif-motion-watch.cc
        onvif-motion-events.cc
    INCLUDES
        ../..
    DEPENDS
        argus::camera-driver
        argus::camera-motion
        argus::camera-onvif
        argus::camera-repositories
        Drogon::Drogon)

target_compile_options(argus_camera-onvif-events PRIVATE -Wall -Wextra)
