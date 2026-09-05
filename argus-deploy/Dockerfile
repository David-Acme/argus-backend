# syntax=docker/dockerfile:1.7

# One source-built image for the cutover stack (Ruling N): multi-stage build of
# the legacy backend, the gateway, argus-camera and the migration tools from
# the same source tree. Extends docker/Dockerfile (same toolchain, Release) and
# keeps the ARGUS_UID/ARGUS_GID runtime pattern.

FROM debian:trixie-slim AS build

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential ca-certificates cmake git libssl-dev ninja-build \
      pkg-config python3 python3-pip python3-venv libvulkan-dev glslc \
    && rm -rf /var/lib/apt/lists/* \
    && python3 -m venv /opt/venv \
    && /opt/venv/bin/pip install --no-cache-dir "conan==2.21.0"

ENV PATH="/opt/venv/bin:${PATH}"

WORKDIR /opt/argus
COPY . .

RUN --mount=type=cache,target=/root/.conan2 \
    cp config.toml.example config.toml \
    && conan profile detect --force \
    && conan install . --output-folder=build/prod -s build_type=Release --build=missing \
    && cmake --preset prod -DARGUS_BUILD_LABS=OFF \
    && cmake --build --preset prod -j"$(nproc)" \
    && cmake --build --preset prod --target argus-gateway argus-migrate-identity \
      argus-vulkan-probe argus-migrate-camera argus-camera -j"$(nproc)"

FROM debian:trixie-slim AS runtime

ARG ARGUS_UID=1000
ARG ARGUS_GID=1000

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
      ca-certificates libgcc-s1 libgomp1 libstdc++6 libvulkan1 \
      mesa-vulkan-drivers curl netcat-openbsd \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /opt/argus
COPY --from=build /opt/argus/build/prod/argus-backend /opt/argus/argus-backend
COPY --from=build /opt/argus/build/prod/argus-gateway/argus-gateway /opt/argus/argus-gateway
COPY --from=build /opt/argus/build/prod/tools/migrate-identity/argus-migrate-identity /opt/argus/argus-migrate-identity
COPY --from=build /opt/argus/build/prod/tools/migrate-camera/argus-migrate-camera /opt/argus/argus-migrate-camera
COPY --from=build /opt/argus/build/prod/tools/vulkan-probe/argus-vulkan-probe /opt/argus/argus-vulkan-probe
COPY --from=build /opt/argus/build/prod/argus-camera/argus-camera /opt/argus/argus-camera
COPY config.toml.example /opt/argus/config.toml

RUN groupadd --gid "$ARGUS_GID" argus \
    && useradd --uid "$ARGUS_UID" --gid "$ARGUS_GID" --create-home \
      --shell /usr/sbin/nologin argus \
    && mkdir -p certs database models third_party/go2rtc camera stream \
    && chown -R argus:argus /opt/argus

EXPOSE 7024
USER argus
ENTRYPOINT ["/opt/argus/argus-backend"]
