argus_module(NAME camera-objects
    SOURCES
        ncnn-object-detector.cc
    INCLUDES
        ../..
    DEPENDS
        argus::hardware-profile
        argus::hardware-profile-gpu
        argus::lib::runtime
        opencv::opencv
        ${ARGUS_NCNN_TARGET}
        Drogon::Drogon)

target_include_directories(argus_camera-objects SYSTEM PUBLIC
    ${NCNN_SRC_DIR}
    ${CMAKE_BINARY_DIR}/third_party/ncnn/src)

target_compile_options(argus_camera-objects PRIVATE -Wall -Wextra)
