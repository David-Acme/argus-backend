# storage: private-object storage over S3 (RustFS backing): signed uploads,
# portrait objects and capability-gated reads. Consumers link argus::storage.

include(${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/argus-module.cmake)

if(NOT TARGET OpenSSL::SSL)
    find_package(OpenSSL REQUIRED)
endif()

if(NOT TARGET Drogon::Drogon)
    find_package(Drogon CONFIG REQUIRED)
endif()

if(NOT TARGET pkg::response)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../response
                     ${CMAKE_CURRENT_BINARY_DIR}/response)
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
    INCLUDES src
    DEPENDS
        pkg::response
        Drogon::Drogon
        argus::config)

target_link_libraries(argus_storage PRIVATE OpenSSL::SSL OpenSSL::Crypto)
target_compile_options(argus_storage PRIVATE -Wall -Wextra)
