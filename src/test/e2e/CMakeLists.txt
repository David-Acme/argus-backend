add_executable(golden-sync-test golden-sync-test.cc)

target_link_libraries(golden-sync-test PRIVATE Drogon::Drogon)

target_compile_options(golden-sync-test PRIVATE -Wall -Wextra)

add_test(NAME golden-sync-test COMMAND golden-sync-test)