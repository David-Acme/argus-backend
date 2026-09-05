add_executable(argus-object-bench object-bench.cc)

target_link_libraries(argus-object-bench PRIVATE camera-core)

target_compile_options(argus-object-bench PRIVATE -Wall -Wextra
    $<$<CONFIG:Release>:-O3;-march=native;-flto=auto>)
target_link_options(argus-object-bench PRIVATE $<$<CONFIG:Release>:-flto=auto>)
