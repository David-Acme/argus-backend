argus_module(NAME camera-health
    SOURCES
        health-rpc-service.cc
    INCLUDES
        ../..
    DEPENDS
        argus::lib::grpc-health)

target_compile_options(argus_camera-health PRIVATE -Wall -Wextra)
