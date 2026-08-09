add_executable(argus-audio-probe
    audio-probe.cc
    ${SRC_ROOT}/shared/wrapper/audio/audio-resampler.cc
)
target_include_directories(argus-audio-probe PRIVATE ${SRC_ROOT})
target_compile_options(argus-audio-probe PRIVATE -Wall -Wextra
    $<$<CONFIG:Release>:-O3;-march=native;-flto=auto>)
target_link_options(argus-audio-probe PRIVATE $<$<CONFIG:Release>:-flto=auto>)
