argus_module(NAME camera-repositories
    SOURCES
        camera/camera-repository.cc
        camera-stream/camera-stream-repository.cc
        zone/zone-repository.cc
        ../schemas/camera/camera-schema.cc
        ../schemas/camera-stream/camera-stream-schema.cc
        ../schemas/zone/zone-schema.cc
    DEPENDS
        argus::contracts::camera
        argus::contracts::sync
        argus::lib::sqlite
    INCLUDES
        ${CMAKE_CURRENT_SOURCE_DIR}/../..)

target_compile_options(argus_camera-repositories PRIVATE -Wall -Wextra)
