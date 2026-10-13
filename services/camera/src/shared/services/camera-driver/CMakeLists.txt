argus_module(NAME camera-driver
    SOURCES
        camera-capabilities.cc
        camera-driver-registry.cc
        camera-scene-log.cc
        capability-record.cc
        onvif-driver.cc
        stream-only-driver.cc
        tapo-driver.cc
    INCLUDES
        ../../..
    DEPENDS
        argus::camera-catalog
        argus::camera-onvif
        argus::camera-repositories
        argus::camera-tapo
        argus::contracts::sync
        argus::lib::config
        argus::lib::runtime
        argus::lib::sqlite
        argus::lib::text
        Drogon::Drogon
        nlohmann_json::nlohmann_json)

target_compile_options(argus_camera-driver PRIVATE -Wall -Wextra)
