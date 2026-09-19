# threading: concurrency primitives (off-loop blocking tasks, cancellation,
# adaptive thread budgets, the shared AI-engine mutex). Consumers link
# argus::threading.

include(${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/argus-module.cmake)

if(NOT TARGET Drogon::Drogon)
    find_package(Drogon CONFIG REQUIRED)
endif()

argus_module(NAME threading
    SOURCES
        src/shared/wrapper/thread-budget/thread-budget.cc
        src/shared/wrapper/thread-budget/thread-budget.hxx
        src/shared/wrapper/ai-init/ai-init.hxx
        src/shared/wrapper/blocking-task/blocking-task.hxx
        src/shared/wrapper/cancellation/cancellation-token.hxx
    INCLUDES src
    DEPENDS Drogon::Drogon)

target_compile_options(argus_threading PRIVATE -Wall -Wextra)

enable_testing()
find_package(doctest REQUIRED)

add_executable(thread-budget-test tests/unit/thread-budget-test.cc)
target_link_libraries(thread-budget-test PRIVATE argus_threading
    doctest::doctest)
target_compile_options(thread-budget-test PRIVATE -Wall -Wextra)
add_test(NAME thread-budget-test COMMAND thread-budget-test)

get_target_property(DOCTEST_INCLUDE_DIRS doctest::doctest
                    INTERFACE_INCLUDE_DIRECTORIES)
if(DOCTEST_INCLUDE_DIRS)
    set_target_properties(doctest::doctest PROPERTIES
        INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${DOCTEST_INCLUDE_DIRS}")
endif()
