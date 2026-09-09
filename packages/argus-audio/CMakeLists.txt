# argus_audio: the PCM helpers more than one service needs. Today that is the
# resampler argus-camera's Tapo talk path and argus-voice's session both run;
# it is a module rather than either service's source so neither has to reach
# into the other's src/.

include(${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/argus-module.cmake)

argus_module(NAME audio
    SOURCES
        src/shared/wrapper/audio/audio-resampler.cc
    INCLUDES
        src
    DEPENDS
        argus_common)

target_compile_options(argus_audio PRIVATE -Wall -Wextra)
