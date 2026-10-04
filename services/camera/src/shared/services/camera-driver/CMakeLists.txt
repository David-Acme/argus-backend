argus_module(NAME camera-driver
    SOURCES
        camera-driver-registry.cc
        camera-scene-log.cc
        stream-only-driver.cc
        tapo-driver.cc
    INCLUDES
        ../../..
    DEPENDS
        argus::camera-catalog
        argus::camera-repositories
        argus::camera-tapo
        argus::lib::config
        argus::lib::runtime
        argus::lib::text
        Drogon::Drogon
        nlohmann_json::nlohmann_json)

target_compile_options(argus_camera-driver PRIVATE -Wall -Wextra)
