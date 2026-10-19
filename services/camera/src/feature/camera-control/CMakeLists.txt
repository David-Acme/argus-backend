argus_module(NAME camera-control
    SOURCES
        controllers/camera-control-controller.cc
        dtos/camera-aim-dto.cc
        dtos/camera-preset-dto.cc
        dtos/camera-ptz-dto.cc
        dtos/camera-sd-dto.cc
        dtos/camera-settings-dto.cc
        dtos/camera-snapshot-dto.cc
        dtos/camera-talk-dto.cc
        services/aim-board.cc
        services/aim-math.cc
        services/aim-settings.cc
        services/camera-control-feature-service.cc
        services/duty-gate.cc
        services/ptz-lease.cc
        services/still-flights.cc
    INCLUDES
        ../..
    DEPENDS
        argus::camera-driver
        argus::camera-privacy
        argus::camera-privacy-state
        argus::camera-repositories
        argus::camera-stream
        argus::clients::guard
        argus::clients::tts
        argus::contracts::camera
        argus::lib::auth
        argus::lib::errors
        argus::lib::http
        argus::lib::runtime
        argus::lib::validation
        Drogon::Drogon)

target_compile_options(argus_camera-control PRIVATE -Wall -Wextra)
