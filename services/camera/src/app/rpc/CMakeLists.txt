argus_module(NAME camera-rpc-server
    SOURCES
        camera-rpc-server.cc
    INCLUDES
        ../..
    DEPENDS
        argus::lib::grpc-health
        argus::lib::http
        Drogon::Drogon)

target_compile_options(argus_camera-rpc-server PRIVATE -Wall -Wextra)
