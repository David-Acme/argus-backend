argus_module(NAME camera-zone
    SOURCES
        controllers/zone-controller.cc
        dtos/create-zone-dto.cc
        dtos/update-zone-dto.cc
        services/zone-feature-service.cc
    INCLUDES
        ../..
    DEPENDS
        argus::camera-repositories
        argus::contracts::camera
        argus::contracts::sync
        argus::lib::errors
        argus::lib::http
        argus::lib::runtime
        argus::lib::sqlite
        argus::lib::validation
        Drogon::Drogon)

target_compile_options(argus_camera-zone PRIVATE -Wall -Wextra)
