# syntax=docker/dockerfile:1.7

# One source-built image for the cutover stack (Ruling N): multi-stage build of
# the gateway, the domain services and the migration tools from the same
# source tree (same toolchain, Release), keeping the ARGUS_UID/ARGUS_GID
# runtime pattern.

FROM debian:trixie-slim AS build

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential ca-certificates cmake git libssl-dev ninja-build \
      pkg-config python3 python3-pip python3-venv libvulkan-dev glslc \
      libprotobuf-dev protobuf-compiler protobuf-compiler-grpc \
      libgrpc++-dev libgrpc-dev \
    && rm -rf /var/lib/apt/lists/* \
    && python3 -m venv /opt/venv \
    && /opt/venv/bin/pip install --no-cache-dir "conan==2.21.0"

ENV PATH="/opt/venv/bin:${PATH}"

WORKDIR /opt/argus
COPY . .

RUN --mount=type=cache,target=/root/.conan2 \
    cp config.toml.example config.toml \
    && conan profile detect --force \
    && printf '\n[replace_requires]\nprotobuf/*: protobuf/3.21.12\n' \
      >> /root/.conan2/profiles/default \
    && conan install . --output-folder=build/prod -s build_type=Release --build=missing \
    && cmake --preset prod -DARGUS_BUILD_LABS=OFF -DARGUS_SYSTEM_PROTOBUF=ON \
    && cmake --build --preset prod -j"$(nproc)" \
    && cmake --build --preset prod --target argus-gateway argus-migrate-identity \
      argus-vulkan-probe argus-migrate-camera argus-camera \
      argus-productivity argus-notification argus-migrate-productivity \
      argus-migrate-notification argus-tts argus-stt argus-vlm argus-llm \
      argus-memory argus-voice argus-tunnel-client argus-tunnel-relay -j"$(nproc)"

FROM debian:trixie-slim AS runtime

ARG ARGUS_UID=1000
ARG ARGUS_GID=1000

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
      ca-certificates libgcc-s1 libgomp1 libstdc++6 libvulkan1 \
      mesa-vulkan-drivers curl \
      libgrpc++1.51t64 libgrpc29t64 \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /opt/argus
COPY --from=build /opt/argus/build/prod/argus-gateway/argus-gateway /opt/argus/argus-gateway
COPY --from=build /opt/argus/build/prod/argus-identity/tools/migrate-identity/argus-migrate-identity /opt/argus/argus-migrate-identity
COPY --from=build /opt/argus/build/prod/argus-camera/tools/migrate-camera/argus-migrate-camera /opt/argus/argus-migrate-camera
COPY --from=build /opt/argus/build/prod/argus-camera/tools/vulkan-probe/argus-vulkan-probe /opt/argus/argus-vulkan-probe
COPY --from=build /opt/argus/build/prod/argus-camera/argus-camera /opt/argus/argus-camera
COPY --from=build /opt/argus/build/prod/argus-productivity/argus-productivity /opt/argus/argus-productivity
COPY --from=build /opt/argus/build/prod/argus-notification/argus-notification /opt/argus/argus-notification
COPY --from=build /opt/argus/build/prod/argus-productivity/tools/migrate-productivity/argus-migrate-productivity /opt/argus/argus-migrate-productivity
COPY --from=build /opt/argus/build/prod/argus-notification/tools/migrate-notification/argus-migrate-notification /opt/argus/argus-migrate-notification
COPY --from=build /opt/argus/build/prod/argus-tts/argus-tts /opt/argus/argus-tts
COPY --from=build /opt/argus/build/prod/argus-stt/argus-stt /opt/argus/argus-stt
COPY --from=build /opt/argus/build/prod/argus-vlm/argus-vlm /opt/argus/argus-vlm
COPY --from=build /opt/argus/build/prod/argus-llm/argus-llm /opt/argus/argus-llm
COPY --from=build /opt/argus/build/prod/argus-memory/argus-memory /opt/argus/argus-memory
COPY --from=build /opt/argus/build/prod/argus-voice/argus-voice /opt/argus/argus-voice
COPY --from=build /opt/argus/build/prod/argus-tunnel/argus-tunnel-client /opt/argus/argus-tunnel-client
COPY --from=build /opt/argus/build/prod/argus-tunnel/argus-tunnel-relay /opt/argus/argus-tunnel-relay
COPY config.toml.example /opt/argus/config.toml

RUN groupadd --gid "$ARGUS_GID" argus \
    && useradd --uid "$ARGUS_UID" --gid "$ARGUS_GID" --create-home \
      --shell /usr/sbin/nologin argus \
    && mkdir -p certs database models third_party/go2rtc camera stream \
      productivity notification memory \
    && chown -R argus:argus /opt/argus

EXPOSE 7024
USER argus
ENTRYPOINT ["/opt/argus/argus-gateway"]
