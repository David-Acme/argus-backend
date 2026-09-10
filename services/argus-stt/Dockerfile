# syntax=docker/dockerfile:1.7

# argus-stt service image. Build from the repository root:
#   docker build -f services/argus-stt/Dockerfile -t argus-stt:local .

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

RUN conan profile detect --force \
    && printf '\n[replace_requires]\nprotobuf/*: protobuf/3.21.12\n' \
      >> /root/.conan2/profiles/default

RUN --mount=type=cache,target=/root/.conan2/p \
    --mount=type=cache,target=/root/.conan2/b \
    ./scripts/build-all.sh prod --no-tests --only argus-stt

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
COPY --from=build /opt/argus/services/argus-stt/build/prod/argus-stt /opt/argus/argus-stt

RUN groupadd --gid "$ARGUS_GID" argus \
    && useradd --uid "$ARGUS_UID" --gid "$ARGUS_GID" --create-home \
      --shell /usr/sbin/nologin argus \
    && mkdir -p certs database models third_party/go2rtc camera stream \
      productivity notification memory \
    && chown -R argus:argus /opt/argus

EXPOSE 7030
USER argus
ENTRYPOINT ["/opt/argus/argus-stt"]
