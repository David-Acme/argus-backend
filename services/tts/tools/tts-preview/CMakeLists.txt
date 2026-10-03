add_executable(argus-tts-preview tts-preview.cc)

target_link_libraries(argus-tts-preview PRIVATE argus::tts-synthesis)

target_compile_options(argus-tts-preview PRIVATE -Wall -Wextra)

argus_runtime_rpath(argus-tts-preview)
