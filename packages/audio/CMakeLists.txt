# argus_audio: the PCM helpers more than one service needs. Today that is the
# resampler argus-camera's Tapo talk path and argus-voice's session both run;
# it is a module rather than either service's source so neither has to reach
# into the other's src/.

include(${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/argus-module.cmake)

argus_module(NAME audio
    SOURCES
        src/shared/wrapper/audio/audio-resampler.cc
        src/shared/wrapper/audio/endpoint-detector.cc
    INCLUDES
        src
    DEPENDS
        argus_common)

target_compile_options(argus_audio PRIVATE -Wall -Wextra)

enable_testing()
find_package(doctest REQUIRED)

add_executable(endpoint-detector-test
    tests/unit/endpoint-detector-test.cc)
target_link_libraries(endpoint-detector-test PRIVATE argus::audio
    doctest::doctest)
target_compile_options(endpoint-detector-test PRIVATE -Wall -Wextra)
add_test(NAME endpoint-detector-test COMMAND endpoint-detector-test)
