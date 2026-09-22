# The three audit tables' row code as one module: the repositories and the
# schemas they map rows through. Declared together rather than inside a
# repository folder because the transport's audit pages and the fan-out's writes
# read them as one set, and because the schemas' translation units have no other
# compiler. The fourth table, the delivery inbox, has one reader — its consumer
# in `fanout` — so that repository is compiled there (rule 23).
argus_module(NAME sync-repositories
    SOURCES
        audit-log/audit-log-repository.cc
        user-audit-log/user-audit-log-repository.cc
        user-action-log/user-action-log-repository.cc
        ../schemas/audit-log/audit-log-schema.cc
        ../schemas/user-audit-log/user-audit-log-schema.cc
        ../schemas/user-action-log/user-action-log-schema.cc
    INCLUDES ../..
    DEPENDS
        argus::contracts::sync
        argus::lib::sqlite
        argus::lib::text
        Drogon::Drogon)

target_compile_options(argus_sync-repositories PRIVATE -Wall -Wextra)
