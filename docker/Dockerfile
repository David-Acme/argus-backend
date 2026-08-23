# syntax=docker/dockerfile:1.7

# The backend image is optional. Native development should keep running the
# executable directly; this image is for a portable, release-mode deployment.
FROM ubuntu:24.04 AS build

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential ca-certificates cmake git libssl-dev ninja-build \
      pkg-config python3 python3-pip \
    && rm -rf /var/lib/apt/lists/* \
    && python3 -m pip install --no-cache-dir "conan==2.20.0"

WORKDIR /opt/argus
COPY . .

RUN conan profile detect --force \
    && conan install . --output-folder=build/prod -s build_type=Release --build=missing \
    && cmake --preset prod \
    && cmake --build --preset prod -j"$(nproc)"

FROM ubuntu:24.04 AS runtime

ARG ARGUS_UID=1000
ARG ARGUS_GID=1000

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
      ca-certificates libgomp1 libvulkan1 \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /opt/argus
COPY --from=build /opt/argus/build/prod/argus-backend /opt/argus/argus-backend
COPY config.toml /opt/argus/config.toml

RUN groupadd --gid "$ARGUS_GID" argus \
    && useradd --uid "$ARGUS_UID" --gid "$ARGUS_GID" --create-home \
      --shell /usr/sbin/nologin argus \
    && mkdir -p certs database models third_party/go2rtc uploads \
    && chown -R argus:argus /opt/argus

EXPOSE 7024
USER argus
ENTRYPOINT ["/opt/argus/argus-backend"]
