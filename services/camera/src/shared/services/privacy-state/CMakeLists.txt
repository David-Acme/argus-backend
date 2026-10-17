argus_module(NAME camera-privacy-state
    SOURCES
        privacy-state.cc
    INCLUDES
        ../../..
    DEPENDS
        argus::camera-repositories
        argus::camera-stream
        argus::contracts::camera
        argus::contracts::sync
        Drogon::Drogon)

target_compile_options(argus_camera-privacy-state PRIVATE -Wall -Wextra)
