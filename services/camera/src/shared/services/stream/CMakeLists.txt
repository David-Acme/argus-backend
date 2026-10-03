argus_module(NAME camera-stream
    SOURCES
        camera-source-registrar.cc
        gop-cache.cc
        go2rtc-manager.cc
        snapshot-store.cc
        stream-hub.cc
        upstream-http.cc
        ws-frame.cc
    INCLUDES
        ../../..
    DEPENDS
        argus::camera-repositories
        argus::lib::config
        Drogon::Drogon)

target_compile_options(argus_camera-stream PRIVATE -Wall -Wextra)
