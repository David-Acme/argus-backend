# argus_sqlite: the SQLite substrate — DbService boot, VecDb and the vec0
# vector-index repository. Consumers link argus::sqlite.

find_package(Drogon CONFIG REQUIRED)

argus_module(NAME sqlite
    SOURCES
        src/shared/services/sqlite/db-service.cc
        src/shared/services/sqlite/vec-db.cc
        src/shared/repositories/vector-index/vector-index-repository.cc
    INCLUDES src)

target_link_libraries(argus_sqlite
    PUBLIC
        argus_common
        Drogon::Drogon)

if(ARGUS_SQLITE_VEC_TARGET AND TARGET ${ARGUS_SQLITE_VEC_TARGET})
    target_link_libraries(argus_sqlite PUBLIC ${ARGUS_SQLITE_VEC_TARGET})
endif()

target_compile_options(argus_sqlite PRIVATE -Wall -Wextra)

# The named identity client slot: installed at boot, falling back to the
# default client when the host installs none.
if(ARGUS_ROOT_PROJECT)
    find_package(doctest REQUIRED)

    add_executable(identity-client-test tests/unit/identity-client-test.cc)
    target_link_libraries(identity-client-test PRIVATE argus::sqlite
        argus_common Drogon::Drogon sqlite-vec doctest::doctest)
    target_compile_options(identity-client-test PRIVATE -Wall -Wextra)
    add_test(NAME identity-client-test COMMAND identity-client-test)
endif()
