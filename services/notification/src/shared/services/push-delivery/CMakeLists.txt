find_package(OpenSSL REQUIRED)

argus_module(NAME notification-push-delivery
    SOURCES
        notification-push-device-directory.cc
        push-alert-writer.cc
        push-delivery-wire.cc
        push-sealer.cc
    INCLUDES
        ../../..
    DEPENDS
        argus::contracts::push
        argus::notification-shared
        argus::lib::config
        argus::lib::errors
        argus::lib::nats
        argus::lib::sqlite
        argus::lib::text
        Drogon::Drogon
        OpenSSL::Crypto)

target_compile_options(argus_notification-push-delivery PRIVATE -Wall -Wextra)
