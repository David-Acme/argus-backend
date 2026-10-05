include(${CMAKE_CURRENT_SOURCE_DIR}/../../../cmake/argus-module.cmake)

if(NOT TARGET Drogon::Drogon)
    find_package(Drogon CONFIG REQUIRED)
endif()

argus_lib(NAME runtime
    SOURCES
        src/runtime/thread-budget.cc
        src/runtime/thread-budget.hxx
        src/runtime/cpu-limits.hxx
        src/runtime/shutdown-signal.cc
        src/runtime/shutdown-signal.hxx
        src/runtime/log-output.cc
        src/runtime/log-output.hxx
        src/runtime/ai-init.hxx
        src/runtime/blocking-pool.cc
        src/runtime/blocking-pool.hxx
        src/runtime/blocking-task.hxx
        src/runtime/cancellation-token.hxx
        src/runtime/wake-signal.hxx
    INCLUDES src
    DEPENDS Drogon::Drogon)

target_compile_options(argus_lib_runtime PRIVATE -Wall -Wextra)

argus_module(NAME hardware-profile
    SOURCES
        src/runtime/hardware-profile.cc
        src/runtime/hardware-profile.hxx
        src/runtime/cpu-limits.hxx
    INCLUDES src)

target_compile_definitions(argus_hardware-profile PRIVATE ARGUS_NO_NCNN_GPU=1)
target_compile_options(argus_hardware-profile PRIVATE -Wall -Wextra)

argus_module(NAME hardware-profile-gpu
    SOURCES
        src/runtime/hardware-profile.cc
        src/runtime/hardware-profile.hxx
        src/runtime/cpu-limits.hxx
    INCLUDES src)

target_compile_options(argus_hardware-profile-gpu PRIVATE -Wall -Wextra)

if(ARGUS_NCNN_TARGET AND TARGET ${ARGUS_NCNN_TARGET})
    target_link_libraries(argus_hardware-profile-gpu PUBLIC ${ARGUS_NCNN_TARGET})
endif()

enable_testing()
find_package(doctest REQUIRED)

add_executable(thread-budget-test tests/unit/thread-budget-test.cc)
target_link_libraries(thread-budget-test PRIVATE argus::lib::runtime
    doctest::doctest)
target_compile_options(thread-budget-test PRIVATE -Wall -Wextra)
add_test(NAME thread-budget-test COMMAND thread-budget-test)

add_executable(cpu-limits-test tests/unit/cpu-limits-test.cc)
target_link_libraries(cpu-limits-test PRIVATE argus::lib::runtime
    doctest::doctest)
target_compile_options(cpu-limits-test PRIVATE -Wall -Wextra)
add_test(NAME cpu-limits-test COMMAND cpu-limits-test)

add_executable(shutdown-deadline-test tests/unit/shutdown-deadline-test.cc)
target_link_libraries(shutdown-deadline-test PRIVATE argus::lib::runtime
    doctest::doctest)
target_compile_options(shutdown-deadline-test PRIVATE -Wall -Wextra)
add_test(NAME shutdown-deadline-test COMMAND shutdown-deadline-test)

add_executable(shutdown-signal-test tests/unit/shutdown-signal-test.cc)
target_link_libraries(shutdown-signal-test PRIVATE argus::lib::runtime
    doctest::doctest)
target_compile_options(shutdown-signal-test PRIVATE -Wall -Wextra)
add_test(NAME shutdown-signal-test COMMAND shutdown-signal-test)

add_executable(shutdown-quit-hook-test tests/unit/shutdown-quit-hook-test.cc)
target_link_libraries(shutdown-quit-hook-test PRIVATE argus::lib::runtime
    doctest::doctest)
target_compile_options(shutdown-quit-hook-test PRIVATE -Wall -Wextra)
add_test(NAME shutdown-quit-hook-test COMMAND shutdown-quit-hook-test)

add_executable(log-output-test tests/unit/log-output-test.cc)
target_link_libraries(log-output-test PRIVATE argus::lib::runtime
    doctest::doctest)
target_compile_options(log-output-test PRIVATE -Wall -Wextra)
add_test(NAME log-output-test COMMAND log-output-test)

add_executable(wake-signal-test tests/unit/wake-signal-test.cc)
target_link_libraries(wake-signal-test PRIVATE argus::lib::runtime
    doctest::doctest)
target_compile_options(wake-signal-test PRIVATE -Wall -Wextra)
add_test(NAME wake-signal-test COMMAND wake-signal-test)

add_executable(blocking-pool-test tests/unit/blocking-pool-test.cc)
target_link_libraries(blocking-pool-test PRIVATE argus::lib::runtime
    doctest::doctest)
target_compile_options(blocking-pool-test PRIVATE -Wall -Wextra)
add_test(NAME blocking-pool-test COMMAND blocking-pool-test)

add_executable(blocking-task-test tests/unit/blocking-task-test.cc)
target_link_libraries(blocking-task-test PRIVATE argus::lib::runtime
    doctest::doctest)
target_compile_options(blocking-task-test PRIVATE -Wall -Wextra)
add_test(NAME blocking-task-test COMMAND blocking-task-test)

add_executable(blocking-pool-stress-test tests/unit/blocking-pool-stress-test.cc)
target_link_libraries(blocking-pool-stress-test PRIVATE argus::lib::runtime
    doctest::doctest)
target_compile_options(blocking-pool-stress-test PRIVATE -Wall -Wextra)
add_test(NAME blocking-pool-stress-test COMMAND blocking-pool-stress-test)

get_target_property(DOCTEST_INCLUDE_DIRS doctest::doctest
                    INTERFACE_INCLUDE_DIRECTORIES)
if(DOCTEST_INCLUDE_DIRS)
    set_target_properties(doctest::doctest PROPERTIES
        INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${DOCTEST_INCLUDE_DIRS}")
endif()
