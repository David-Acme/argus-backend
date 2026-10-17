argus_module(NAME camera-privacy-notice
    SOURCES
        privacy-notice.cc
    INCLUDES
        ../../..
    DEPENDS
        argus::camera-config
        argus::clients::identity
        argus::clients::notification
        argus::lib::runtime
        argus::lib::text
        Drogon::Drogon)

target_compile_options(argus_camera-privacy-notice PRIVATE -Wall -Wextra)
