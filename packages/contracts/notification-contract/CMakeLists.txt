# notification-contract: the vocabulary that crosses the notification
# boundary — the delivery status a notification reports back through the sync
# leg. Declared once, here, for the service that owns the boundary and for the
# packages and services that consume it. Header-only; consumers link
# contract::notification.

cmake_minimum_required(VERSION 3.25)

add_library(contract-notification INTERFACE)
add_library(contract::notification ALIAS contract-notification)
target_include_directories(contract-notification INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR})
target_sources(contract-notification INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/notification-delivery-status.hxx)
