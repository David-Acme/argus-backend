argus_module(NAME camera-catalog
    SOURCES
        camera-catalog.cc
    INCLUDES
        ../../..
    DEPENDS
        argus::contracts::camera
        Drogon::Drogon)

target_compile_options(argus_camera-catalog PRIVATE -Wall -Wextra)
