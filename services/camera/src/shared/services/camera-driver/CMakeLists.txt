argus_module(NAME camera-driver
    SOURCES
        camera-driver-registry.cc
        tapo-driver.cc
    INCLUDES
        ../../..
    DEPENDS
        argus::camera-repositories
        argus::camera-tapo
        argus::lib::config
        argus::lib::runtime
        Drogon::Drogon
        nlohmann_json::nlohmann_json)

target_compile_options(argus_camera-driver PRIVATE -Wall -Wextra)
