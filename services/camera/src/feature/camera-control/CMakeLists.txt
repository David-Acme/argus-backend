argus_module(NAME camera-control
    SOURCES
        controllers/camera-control-controller.cc
        dtos/camera-preset-dto.cc
        dtos/camera-ptz-dto.cc
        dtos/camera-settings-dto.cc
        dtos/camera-talk-dto.cc
        services/camera-control-feature-service.cc
    INCLUDES
        ../..
    DEPENDS
        argus::camera-driver
        argus::camera-repositories
        argus::clients::tts
        argus::contracts::camera
        argus::lib::errors
        argus::lib::http
        argus::lib::runtime
        argus::lib::validation
        Drogon::Drogon)

target_compile_options(argus_camera-control PRIVATE -Wall -Wextra)
