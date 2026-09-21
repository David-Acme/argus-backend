# storage: private-object storage over S3 (RustFS backing): signed uploads,
# portrait objects and capability-gated reads. Consumers link argus::storage.

include(${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/argus-module.cmake)

if(NOT TARGET OpenSSL::SSL)
    find_package(OpenSSL REQUIRED)
endif()

if(NOT TARGET Drogon::Drogon)
    find_package(Drogon CONFIG REQUIRED)
endif()

if(NOT TARGET argus::errors)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../errors
                     ${CMAKE_CURRENT_BINARY_DIR}/errors)
endif()

if(NOT TARGET argus::config)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../config
                     ${CMAKE_CURRENT_BINARY_DIR}/config)
endif()

argus_module(NAME storage
    SOURCES
        src/shared/services/storage/s3-signing.hxx
        src/shared/services/storage/s3-storage-service.cc
        src/shared/services/storage/s3-storage-service.hxx
        src/shared/services/storage/storage-errors.hxx
        src/shared/vocabulary/stored-file-category.hxx
    INCLUDES src
    DEPENDS
        argus::errors
        Drogon::Drogon
        argus::config)

target_link_libraries(argus_storage PRIVATE OpenSSL::SSL OpenSSL::Crypto)
target_compile_options(argus_storage PRIVATE -Wall -Wextra)

# The round-trip suite for StoredFileCategory: the vocabulary moved here with
# the package that owns the stored objects, so the suite moves with it.
enable_testing()
find_package(doctest REQUIRED)

add_executable(storage-vocabulary-test tests/unit/storage-vocabulary-test.cc)
target_link_libraries(storage-vocabulary-test PRIVATE
    argus::storage doctest::doctest)
target_compile_options(storage-vocabulary-test PRIVATE -Wall -Wextra)
add_test(NAME storage-vocabulary-test COMMAND storage-vocabulary-test)

get_target_property(DOCTEST_INCLUDE_DIRS doctest::doctest
                    INTERFACE_INCLUDE_DIRECTORIES)
if(DOCTEST_INCLUDE_DIRS)
    set_target_properties(doctest::doctest PROPERTIES
        INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${DOCTEST_INCLUDE_DIRS}")
endif()
