argus_module(NAME camera-sync
    SOURCES
        camera-sync-rpc-service.cc
        repositories/camera-stream/camera-stream-repository.cc
        schemas/camera-stream/camera-stream-schema.cc
    INCLUDES
        ../..
    DEPENDS
        argus::camera-repositories
        argus::clients::camera
        argus::contracts::auth
        argus::contracts::camera
        argus::contracts::sync
        argus::lib::auth
        argus::lib::sqlite
        Drogon::Drogon)

target_compile_options(argus_camera-sync PRIVATE -Wall -Wextra)
