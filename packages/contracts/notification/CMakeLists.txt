# notification-contract: the vocabulary that crosses the notification
# boundary — the delivery status a notification reports back through the sync
# leg. Declared once, here, for the service that owns the boundary and for the
# packages and services that consume it. Header-only; consumers link
# argus::contracts::notification.

cmake_minimum_required(VERSION 3.25)

include(${CMAKE_CURRENT_SOURCE_DIR}/../../../cmake/argus-module.cmake)

argus_contracts(NAME notification
    INCLUDES .
    SOURCES
        notification-delivery-status.hxx)
