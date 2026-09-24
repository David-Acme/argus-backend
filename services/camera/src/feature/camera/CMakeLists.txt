argus_module(NAME camera-feature
    SOURCES
        controllers/camera-controller.cc
        dtos/create-camera-dto.cc
        dtos/update-camera-dto.cc
        services/camera-feature-service.cc
    INCLUDES
        ../..
    DEPENDS
        argus::camera-repositories
        argus::camera-stream
        argus::contracts::camera
        argus::contracts::sync
        argus::lib::errors
        argus::lib::http
        argus::lib::runtime
        argus::lib::sqlite
        argus::lib::validation
        Drogon::Drogon)

target_compile_options(argus_camera-feature PRIVATE -Wall -Wextra)
