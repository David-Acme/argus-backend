# The camera domain's row code as one module: the three synced tables'
# repositories and the schemas they map rows through. Declared here rather than
# inside a repository folder because the three are one readership — the whole
# service writes them and no target reads one without the others — and because
# the schemas' translation units have no other compiler (they rode argus_sync
# until sub-step 3a-1b).
argus_module(NAME camera-repositories
    SOURCES
        camera/camera-repository.cc
        camera-stream/camera-stream-repository.cc
        zone/zone-repository.cc
        ../schemas/camera/camera-schema.cc
        ../schemas/camera-stream/camera-stream-schema.cc
        ../schemas/zone/zone-schema.cc
    DEPENDS
        argus::contracts::camera
        argus::contracts::sync
        argus::lib::sqlite
    INCLUDES
        ${CMAKE_CURRENT_SOURCE_DIR}/../..)

target_compile_options(argus_camera-repositories PRIVATE -Wall -Wextra)
