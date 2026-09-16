argus_module(NAME guard-api
    SOURCES
        controllers/guard-controller.cc
        services/guard-feature-service.cc
        dtos/create-expected-guest-dto.cc
        dtos/feedback-decision-dto.cc
        dtos/list-decisions-dto.cc
        dtos/summary-decisions-dto.cc
        dtos/list-incidents-dto.cc
        dtos/remove-expected-guest-dto.cc
        dtos/update-guard-mode-dto.cc
    DEPENDS
        argus_common
        argus::guard
        argus::auth
        argus::sdk-identity
    INCLUDES
        ${CMAKE_CURRENT_SOURCE_DIR}/../../..)
