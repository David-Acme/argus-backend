argus_module(NAME guard-proximity
    SOURCES
        controllers/proximity-controller.cc
        dtos/create-proximity-receiver-dto.cc
        dtos/create-proximity-sensor-dto.cc
        dtos/response-proximity-dto.cc
        dtos/update-proximity-me-dto.cc
        infra/fleet-proximity-world.cc
        infra/http-proximity-probe.cc
        infra/proximity-readings.cc
        repositories/proximity-beacon/proximity-beacon-repository.cc
        repositories/proximity-receiver/proximity-receiver-repository.cc
        repositories/proximity-sensor/proximity-sensor-repository.cc
        services/beacon-token.cc
        services/proximity-service.cc
        services/room-belief.cc
        services/whereabouts.cc
    INCLUDES
        ../..
    DEPENDS
        argus::guard-config
        argus::clients::camera-watch
        argus::clients::identity
        argus::contracts::auth
        argus::lib::auth
        argus::lib::errors
        argus::lib::http
        argus::lib::runtime
        argus::lib::sqlite
        argus::lib::text
        argus::lib::validation
        Drogon::Drogon
        OpenSSL::Crypto)

target_compile_options(argus_guard-proximity PRIVATE -Wall -Wextra)
