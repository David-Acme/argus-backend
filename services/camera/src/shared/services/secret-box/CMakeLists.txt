argus_module(NAME camera-secret-box
    SOURCES
        secret-box.cc
    INCLUDES
        ../../..
    DEPENDS
        argus::lib::text
        OpenSSL::Crypto)

target_compile_options(argus_camera-secret-box PRIVATE -Wall -Wextra)
