argus_module(NAME productivity-rpc-server
    SOURCES
        productivity-rpc-server.cc
    INCLUDES
        ../..
    DEPENDS
        argus::lib::grpc-health
        argus::lib::http
        Drogon::Drogon)

target_compile_options(argus_productivity-rpc-server PRIVATE -Wall -Wextra)
