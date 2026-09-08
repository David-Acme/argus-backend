# argus_sqlite: the SQLite substrate — DbService boot, VecDb and the vec0
# vector-index repository. Consumers link argus::sqlite.

find_package(Drogon CONFIG REQUIRED)

argus_module(NAME sqlite
    SOURCES
        ../shared/services/sqlite/db-service.cc
        ../shared/services/sqlite/vec-db.cc
        ../shared/repositories/vector-index/vector-index-repository.cc
    INCLUDES ..)

target_link_libraries(argus_sqlite
    PUBLIC
        argus_common
        Drogon::Drogon)

if(ARGUS_SQLITE_VEC_TARGET AND TARGET ${ARGUS_SQLITE_VEC_TARGET})
    target_link_libraries(argus_sqlite PUBLIC ${ARGUS_SQLITE_VEC_TARGET})
endif()

target_compile_options(argus_sqlite PRIVATE -Wall -Wextra)
