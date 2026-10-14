argus_module(NAME camera-motion
    SOURCES
        camera-motion-board.cc
    INCLUDES
        ../../..
    DEPENDS
        Drogon::Drogon)

target_compile_options(argus_camera-motion PRIVATE -Wall -Wextra)
