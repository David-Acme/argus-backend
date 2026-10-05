argus_module(NAME camera-webrtc
    SOURCES
        controllers/camera-webrtc-controller.cc
        dtos/camera-webrtc-offer-dto.cc
        dtos/response-camera-webrtc-dto.cc
        infra/go2rtc-webrtc-gateway.cc
        services/camera-webrtc-service.cc
        services/webrtc-sdp.cc
        services/webrtc-session-closer.cc
    INCLUDES
        ../..
    DEPENDS
        argus::camera-privacy
        argus::camera-repositories
        argus::camera-stream
        argus::contracts::auth
        argus::contracts::camera
        argus::lib::auth
        argus::lib::errors
        argus::lib::http
        argus::lib::runtime
        argus::lib::validation
        Drogon::Drogon)

target_compile_options(argus_camera-webrtc PRIVATE -Wall -Wextra)
