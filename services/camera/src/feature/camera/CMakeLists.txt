argus_module(NAME camera-feature
    SOURCES
        controllers/camera-controller.cc
        dtos/create-camera-dto.cc
        dtos/probe-camera-dto.cc
        dtos/update-camera-dto.cc
        infra/rtsp-probe.cc
        services/camera-feature-service.cc
        services/camera-overview-service.cc
        services/camera-probe-service.cc
    INCLUDES
        ../..
    DEPENDS
        argus::camera-catalog
        argus::camera-driver
        argus::camera-repositories
        argus::camera-stream
        argus::camera-tapo
        argus::contracts::camera
        argus::contracts::sync
        argus::lib::auth
        argus::lib::errors
        argus::lib::http
        argus::lib::runtime
        argus::lib::sqlite
        argus::lib::text
        argus::lib::validation
        Drogon::Drogon)

target_compile_options(argus_camera-feature PRIVATE -Wall -Wextra)
