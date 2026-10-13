argus_module(NAME camera-privacy
    SOURCES
        camera-audio-policy.cc
        privacy-mask.cc
    INCLUDES
        ../../..
    DEPENDS
        argus::clients::identity
        argus::contracts::camera
        argus::lib::runtime
        opencv::opencv
        Drogon::Drogon)

target_compile_options(argus_camera-privacy PRIVATE -Wall -Wextra)
