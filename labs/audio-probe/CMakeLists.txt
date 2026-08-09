add_executable(argus - audio - probe audio -
               probe.cc ${SRC_ROOT} / shared / services / config -
               service / config -
               service.cc ${SRC_ROOT} / shared / wrapper / audio / audio -
               resampler.cc ${SRC_ROOT} / shared / services / vad / vad -
               service.cc) target_include_directories(argus - audio -
                                                      probe PRIVATE ${SRC_ROOT})
    target_link_libraries(argus - audio -
                          probe PRIVATE Drogon::Drogon onnxruntime::onnxruntime
                              tomlplusplus::tomlplusplus ${CONAN_LIBS})
        target_compile_options(argus - audio - probe PRIVATE - Wall - Wextra $ <
                                   $ < CONFIG : Release > : -O3;
                               -march = native; -flto = auto >)
            target_link_options(argus - audio - probe PRIVATE $ < $ <
                                    CONFIG : Release > : -flto = auto >)
                add_custom_command(
                    TARGET argus - audio -
                        probe POST_BUILD COMMAND ${CMAKE_COMMAND} -
                        E create_symlink ${CMAKE_SOURCE_DIR} / models $ <
                    TARGET_FILE_DIR : argus - audio - probe > / models)
