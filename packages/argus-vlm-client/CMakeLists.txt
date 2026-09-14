# argus_vlm-client: the argus-vlm internal wire seen from the caller's side.
# argus-guard is the only consumer; the client owns the envelope and the
# base64 request encoding so no service hand-rolls the wire (rule 23).

include(${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/argus-module.cmake)

find_package(Drogon CONFIG REQUIRED)

argus_module(NAME vlm-client
    SOURCES
        src/shared/services/vision/remote/vlm-client.cc
    INCLUDES
        src
    DEPENDS
        argus_common
        Drogon::Drogon)

target_compile_options(argus_vlm-client PRIVATE -Wall -Wextra)
