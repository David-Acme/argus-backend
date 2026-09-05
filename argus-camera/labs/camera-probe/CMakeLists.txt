add_executable(argus-camera-probe camera-probe.cc)

target_link_libraries(argus-camera-probe PRIVATE camera-core)

target_compile_options(argus-camera-probe PRIVATE -Wall -Wextra
    $<$<CONFIG:Release>:-O3;-march=native;-flto=auto>)
target_link_options(argus-camera-probe PRIVATE $<$<CONFIG:Release>:-flto=auto>)
