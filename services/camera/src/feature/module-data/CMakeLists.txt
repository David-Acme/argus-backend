argus_module(NAME camera-module-data
    SOURCES
        repositories/module-data/module-data-repository.cc
        services/camera-module-data.cc
        services/camera-module-impact.cc
    INCLUDES
        ../..
    DEPENDS
        argus::camera-driver
        argus::camera-stream
        argus::contracts::settings-wire
        argus::lib::auth
        argus::lib::runtime
        argus::lib::sqlite
        argus::lib::storage
        Drogon::Drogon)

target_compile_options(argus_camera-module-data PRIVATE -Wall -Wextra)
