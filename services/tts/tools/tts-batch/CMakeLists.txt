add_executable(argus-tts-batch tts-batch.cc)

target_link_libraries(argus-tts-batch PRIVATE argus::tts-synthesis)

target_compile_options(argus-tts-batch PRIVATE -Wall -Wextra)

argus_runtime_rpath(argus-tts-batch)
