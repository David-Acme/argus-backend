add_executable(argus-tapo-probe tapo-probe.cc)

target_link_libraries(argus-tapo-probe PRIVATE
    argus::camera-tapo
)

target_compile_options(argus-tapo-probe PRIVATE -Wall -Wextra)
