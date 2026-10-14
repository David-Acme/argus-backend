argus_module(NAME camera-onvif
    SOURCES
        onvif-client.cc
        onvif-parse.cc
        onvif-soap.cc
        onvif-transport.cc
    INCLUDES
        ../../..
    DEPENDS
        argus::camera-tapo
        argus::lib::text
        pugixml::pugixml
        Drogon::Drogon)

target_compile_options(argus_camera-onvif PRIVATE -Wall -Wextra)
