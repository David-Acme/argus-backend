argus_module(NAME camera-privacy
    SOURCES
        camera-audio-policy.cc
    INCLUDES
        ../../..
    DEPENDS
        argus::clients::identity
        argus::lib::runtime
        Drogon::Drogon)

target_compile_options(argus_camera-privacy PRIVATE -Wall -Wextra)
