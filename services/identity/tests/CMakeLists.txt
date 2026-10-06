add_executable(identity-config-test unit/identity-config-test.cc)
target_link_libraries(identity-config-test PRIVATE
    argus::identity-config
    argus::lib::http
    doctest::doctest)
target_compile_options(identity-config-test PRIVATE -Wall -Wextra)
argus_runtime_rpath(identity-config-test)
add_test(NAME identity-config-test COMMAND identity-config-test)

add_executable(identity-user-update-test unit/identity-user-update-test.cc)
target_compile_definitions(identity-user-update-test PRIVATE
    ARGUS_IDENTITY_SCHEMA="${CMAKE_CURRENT_SOURCE_DIR}/../database/schema.sql")
target_link_libraries(identity-user-update-test PRIVATE
    argus::identity-user
    argus::lib::auth
    argus::lib::sqlite
    argus::lib::text
    Drogon::Drogon
    doctest::doctest)
target_compile_options(identity-user-update-test PRIVATE -Wall -Wextra)
argus_runtime_rpath(identity-user-update-test)
add_test(NAME identity-user-update-test COMMAND identity-user-update-test)

add_executable(identity-role-input-test unit/identity-role-input-test.cc)
target_link_libraries(identity-role-input-test PRIVATE
    argus::identity-invitation
    argus::identity-user
    argus::lib::validation
    doctest::doctest)
target_compile_options(identity-role-input-test PRIVATE -Wall -Wextra)
argus_runtime_rpath(identity-role-input-test)
add_test(NAME identity-role-input-test COMMAND identity-role-input-test)

add_executable(identity-migration-test unit/identity-migration-test.cc)
target_link_libraries(identity-migration-test PRIVATE
    identity-migration
    doctest::doctest)
target_compile_options(identity-migration-test PRIVATE -Wall -Wextra)
argus_runtime_rpath(identity-migration-test)
add_test(NAME identity-migration-test COMMAND identity-migration-test)

add_executable(identity-role-check-migration-test unit/identity-role-check-migration-test.cc)
target_compile_definitions(identity-role-check-migration-test PRIVATE
    ARGUS_IDENTITY_SCHEMA="${CMAKE_CURRENT_SOURCE_DIR}/../database/schema.sql")
target_link_libraries(identity-role-check-migration-test PRIVATE
    argus::identity-role-storage
    argus::lib::sqlite
    SQLite::SQLite3
    doctest::doctest)
target_compile_options(identity-role-check-migration-test PRIVATE -Wall -Wextra)
argus_runtime_rpath(identity-role-check-migration-test)
add_test(NAME identity-role-check-migration-test COMMAND identity-role-check-migration-test)

add_executable(identity-change-outbox-sink-test
    unit/identity-change-outbox-sink-test.cc)
target_compile_definitions(identity-change-outbox-sink-test PRIVATE
    ARGUS_IDENTITY_SCHEMA="${CMAKE_CURRENT_SOURCE_DIR}/../database/schema.sql")
target_link_libraries(identity-change-outbox-sink-test PRIVATE
    argus::identity-user
    argus::lib::errors
    argus::lib::nats
    argus::lib::sqlite
    argus::contracts::sync
    Drogon::Drogon
    doctest::doctest)
target_compile_options(identity-change-outbox-sink-test PRIVATE -Wall -Wextra)
argus_runtime_rpath(identity-change-outbox-sink-test)
add_test(NAME identity-change-outbox-sink-test
         COMMAND identity-change-outbox-sink-test)

add_executable(identity-sync-rpc-test unit/identity-sync-rpc-test.cc)
target_compile_definitions(identity-sync-rpc-test PRIVATE
    ARGUS_IDENTITY_SCHEMA="${CMAKE_CURRENT_SOURCE_DIR}/../database/schema.sql")
target_link_libraries(identity-sync-rpc-test PRIVATE
    argus::identity-rpc
    argus::clients::identity
    argus::lib::sqlite
    Drogon::Drogon
    doctest::doctest)
target_compile_options(identity-sync-rpc-test PRIVATE -Wall -Wextra)
argus_runtime_rpath(identity-sync-rpc-test)
add_test(NAME identity-sync-rpc-test COMMAND identity-sync-rpc-test)

add_executable(identity-rpc-callers-test unit/identity-rpc-callers-test.cc)
target_link_libraries(identity-rpc-callers-test PRIVATE
    argus::identity-rpc
    argus::clients::identity
    Drogon::Drogon
    doctest::doctest)
target_compile_options(identity-rpc-callers-test PRIVATE -Wall -Wextra)
argus_runtime_rpath(identity-rpc-callers-test)
add_test(NAME identity-rpc-callers-test COMMAND identity-rpc-callers-test)

add_executable(identity-change-transaction-test
    unit/identity-change-transaction-test.cc)
target_compile_definitions(identity-change-transaction-test PRIVATE
    ARGUS_IDENTITY_SCHEMA="${CMAKE_CURRENT_SOURCE_DIR}/../database/schema.sql")
target_link_libraries(identity-change-transaction-test PRIVATE
    argus::identity-invitation
    argus::identity-user
    argus::lib::nats
    argus::lib::sqlite
    argus::contracts::sync
    Drogon::Drogon
    doctest::doctest)
target_compile_options(identity-change-transaction-test PRIVATE -Wall -Wextra)
argus_runtime_rpath(identity-change-transaction-test)
add_test(NAME identity-change-transaction-test
         COMMAND identity-change-transaction-test)

add_executable(identity-face-slots-test unit/identity-face-slots-test.cc)
target_link_libraries(identity-face-slots-test PRIVATE
    argus::identity-shared
    doctest::doctest)
target_compile_options(identity-face-slots-test PRIVATE -Wall -Wextra)
argus_runtime_rpath(identity-face-slots-test)
add_test(NAME identity-face-slots-test COMMAND identity-face-slots-test)

add_executable(identity-candidate-retention-test
    unit/identity-candidate-retention-test.cc)
target_compile_definitions(identity-candidate-retention-test PRIVATE
    ARGUS_IDENTITY_SCHEMA="${CMAKE_CURRENT_SOURCE_DIR}/../database/schema.sql")
target_link_libraries(identity-candidate-retention-test PRIVATE
    argus::identity-retention
    argus::lib::sqlite
    argus::contracts::sync
    Drogon::Drogon
    doctest::doctest)
target_compile_options(identity-candidate-retention-test PRIVATE -Wall -Wextra)
argus_runtime_rpath(identity-candidate-retention-test)
add_test(NAME identity-candidate-retention-test
         COMMAND identity-candidate-retention-test)

add_executable(identity-pairing-banner-test unit/identity-pairing-banner-test.cc)
target_link_libraries(identity-pairing-banner-test PRIVATE
    argus::identity-pairing
    doctest::doctest)
target_compile_options(identity-pairing-banner-test PRIVATE -Wall -Wextra)
argus_runtime_rpath(identity-pairing-banner-test)
add_test(NAME identity-pairing-banner-test COMMAND identity-pairing-banner-test)

add_executable(identity-voiceprint-audio-test
    unit/identity-voiceprint-audio-test.cc)
target_link_libraries(identity-voiceprint-audio-test PRIVATE
    argus::identity-voiceprint
    doctest::doctest)
target_compile_options(identity-voiceprint-audio-test PRIVATE -Wall -Wextra)
argus_runtime_rpath(identity-voiceprint-audio-test)
add_test(NAME identity-voiceprint-audio-test
         COMMAND identity-voiceprint-audio-test)

add_executable(identity-voiceprint-passive-test
    unit/identity-voiceprint-passive-test.cc)
target_link_libraries(identity-voiceprint-passive-test PRIVATE
    argus::identity-voiceprint
    doctest::doctest)
target_compile_options(identity-voiceprint-passive-test PRIVATE -Wall -Wextra)
argus_runtime_rpath(identity-voiceprint-passive-test)
add_test(NAME identity-voiceprint-passive-test
         COMMAND identity-voiceprint-passive-test)

add_executable(identity-voiceprint-test unit/identity-voiceprint-test.cc)
target_compile_definitions(identity-voiceprint-test PRIVATE
    ARGUS_IDENTITY_SCHEMA="${CMAKE_CURRENT_SOURCE_DIR}/../database/schema.sql"
    ARGUS_TEST_SPEAKER_MODEL="${CMAKE_CURRENT_SOURCE_DIR}/../../../models/speaker/3dspeaker_speech_eres2net_sv_en_voxceleb_16k.onnx"
    ARGUS_TEST_VOICE_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/voiceprint")
target_link_libraries(identity-voiceprint-test PRIVATE
    argus::identity-rpc
    argus::identity-voiceprint
    argus::clients::identity
    argus::lib::sqlite
    Drogon::Drogon
    doctest::doctest)
target_compile_options(identity-voiceprint-test PRIVATE -Wall -Wextra)
argus_runtime_rpath(identity-voiceprint-test)
add_test(NAME identity-voiceprint-test COMMAND identity-voiceprint-test)

add_executable(identity-face-embedding-test
    unit/identity-face-embedding-test.cc)
target_compile_definitions(identity-face-embedding-test PRIVATE
    ARGUS_IDENTITY_SCHEMA="${CMAKE_CURRENT_SOURCE_DIR}/../database/schema.sql")
target_link_libraries(identity-face-embedding-test PRIVATE
    argus::identity-shared
    argus::lib::sqlite
    Drogon::Drogon
    doctest::doctest)
target_compile_options(identity-face-embedding-test PRIVATE -Wall -Wextra)
argus_runtime_rpath(identity-face-embedding-test)
add_test(NAME identity-face-embedding-test
         COMMAND identity-face-embedding-test)

add_executable(identity-privacy-test unit/identity-privacy-test.cc)
target_compile_definitions(identity-privacy-test PRIVATE
    ARGUS_IDENTITY_SCHEMA="${CMAKE_CURRENT_SOURCE_DIR}/../database/schema.sql")
target_link_libraries(identity-privacy-test PRIVATE
    argus::identity-privacy
    argus::identity-rpc
    argus::identity-voiceprint
    argus::clients::identity
    argus::lib::auth
    argus::lib::sqlite
    Drogon::Drogon
    doctest::doctest)
target_compile_options(identity-privacy-test PRIVATE -Wall -Wextra)
argus_runtime_rpath(identity-privacy-test)
add_test(NAME identity-privacy-test COMMAND identity-privacy-test)

add_executable(identity-face-model-test unit/identity-face-model-test.cc)
target_compile_definitions(identity-face-model-test PRIVATE
    ARGUS_IDENTITY_SCHEMA="${CMAKE_CURRENT_SOURCE_DIR}/../database/schema.sql"
    ARGUS_TEST_FACE_MODELS="${CMAKE_CURRENT_SOURCE_DIR}/../../../models/face"
    ARGUS_TEST_FACE_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/face")
target_link_libraries(identity-face-model-test PRIVATE
    argus::identity-face-upgrade
    argus::identity-shared
    argus::lib::sqlite
    Drogon::Drogon
    doctest::doctest)
target_compile_options(identity-face-model-test PRIVATE -Wall -Wextra)
argus_runtime_rpath(identity-face-model-test)
add_test(NAME identity-face-model-test COMMAND identity-face-model-test)

add_executable(identity-visitor-policy-test unit/identity-visitor-policy-test.cc)
target_link_libraries(identity-visitor-policy-test PRIVATE
    argus::identity-visitor
    doctest::doctest)
target_compile_options(identity-visitor-policy-test PRIVATE -Wall -Wextra)
argus_runtime_rpath(identity-visitor-policy-test)
add_test(NAME identity-visitor-policy-test COMMAND identity-visitor-policy-test)

add_executable(identity-visitor-test unit/identity-visitor-test.cc)
target_compile_definitions(identity-visitor-test PRIVATE
    ARGUS_IDENTITY_SCHEMA="${CMAKE_CURRENT_SOURCE_DIR}/../database/schema.sql"
    ARGUS_TEST_FACE_MODELS="${CMAKE_CURRENT_SOURCE_DIR}/../../../models/face"
    ARGUS_TEST_FACE_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/face")
target_link_libraries(identity-visitor-test PRIVATE
    argus::identity-visitor
    argus::identity-retention
    argus::identity-shared
    argus::lib::sqlite
    Drogon::Drogon
    doctest::doctest)
target_compile_options(identity-visitor-test PRIVATE -Wall -Wextra)
argus_runtime_rpath(identity-visitor-test)
add_test(NAME identity-visitor-test COMMAND identity-visitor-test)

add_executable(identity-face-guard-test unit/identity-face-guard-test.cc)
target_link_libraries(identity-face-guard-test PRIVATE
    argus::identity-enrollment
    argus::identity-shared
    doctest::doctest)
target_compile_options(identity-face-guard-test PRIVATE -Wall -Wextra)
argus_runtime_rpath(identity-face-guard-test)
add_test(NAME identity-face-guard-test COMMAND identity-face-guard-test)

add_executable(identity-pairing-code-test unit/identity-pairing-code-test.cc)
target_link_libraries(identity-pairing-code-test PRIVATE
    argus::identity-pairing
    argus::identity-rate-gate
    argus::lib::errors
    doctest::doctest)
target_compile_options(identity-pairing-code-test PRIVATE -Wall -Wextra)
argus_runtime_rpath(identity-pairing-code-test)
add_test(NAME identity-pairing-code-test COMMAND identity-pairing-code-test)

add_executable(identity-liveness-test unit/identity-liveness-test.cc)
target_compile_definitions(identity-liveness-test PRIVATE
    ARGUS_TEST_FACE_MODELS="${CMAKE_CURRENT_SOURCE_DIR}/../../../models/face"
    ARGUS_TEST_FACE_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/face")
target_link_libraries(identity-liveness-test PRIVATE
    argus::identity-shared
    argus::lib::sqlite
    doctest::doctest)
target_compile_options(identity-liveness-test PRIVATE -Wall -Wextra)
argus_runtime_rpath(identity-liveness-test)
add_test(NAME identity-liveness-test COMMAND identity-liveness-test)

add_executable(identity-user-safety-test unit/identity-user-safety-test.cc)
target_compile_definitions(identity-user-safety-test PRIVATE
    ARGUS_IDENTITY_SCHEMA="${CMAKE_CURRENT_SOURCE_DIR}/../database/schema.sql")
target_link_libraries(identity-user-safety-test PRIVATE
    argus::identity-enrollment
    argus::identity-invitation
    argus::identity-person
    argus::identity-user
    argus::identity-visitor
    argus::lib::sqlite
    Drogon::Drogon
    doctest::doctest)
target_compile_options(identity-user-safety-test PRIVATE -Wall -Wextra)
argus_runtime_rpath(identity-user-safety-test)
add_test(NAME identity-user-safety-test COMMAND identity-user-safety-test)

add_executable(identity-module-data-test unit/identity-module-data-test.cc)
target_compile_definitions(identity-module-data-test PRIVATE
    ARGUS_IDENTITY_SCHEMA="${CMAKE_CURRENT_SOURCE_DIR}/../database/schema.sql")
target_link_libraries(identity-module-data-test PRIVATE
    argus::identity-module-data
    argus::lib::sqlite
    Drogon::Drogon
    doctest::doctest)
target_compile_options(identity-module-data-test PRIVATE -Wall -Wextra)
argus_runtime_rpath(identity-module-data-test)
add_test(NAME identity-module-data-test COMMAND identity-module-data-test)
