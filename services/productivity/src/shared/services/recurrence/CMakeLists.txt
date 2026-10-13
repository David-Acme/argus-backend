argus_module(NAME productivity-recurrence
    SOURCES
        recurrence.cc
    INCLUDES
        ../../..
    DEPENDS
        Drogon::Drogon)

target_compile_options(argus_productivity-recurrence PRIVATE -Wall -Wextra)
