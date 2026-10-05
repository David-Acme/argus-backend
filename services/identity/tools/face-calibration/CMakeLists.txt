add_executable(argus-face-calibration EXCLUDE_FROM_ALL face-calibration.cc)

target_link_libraries(argus-face-calibration PRIVATE argus::identity-shared)

target_compile_options(argus-face-calibration PRIVATE -Wall -Wextra)

argus_runtime_rpath(argus-face-calibration)
