argus_module(NAME productivity-recurrence
    SOURCES
        occurrence-exceptions.cc
        recurrence.cc
    INCLUDES
        ../../..
    DEPENDS
        Drogon::Drogon)

target_compile_options(argus_productivity-recurrence PRIVATE -Wall -Wextra)
