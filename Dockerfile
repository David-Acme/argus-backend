FROM gcc:14 AS builder

ENV DEBIAN_FRONTEND=noninteractive
ENV PATH="/root/.local/bin:${PATH}"

WORKDIR /app

COPY scripts/ ./scripts/
COPY conanfile.txt CMakeLists.txt CMakePresets.json config.json ./
COPY src ./src

RUN ./scripts/setup-prod.sh

FROM debian:trixie-slim

RUN apt-get update && apt-get install -y --no-install-recommends \
    libssl3 \
    libstdc++6 \
    libgcc-s1 \
    libgomp1 \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY --from=builder /app/build/prod/argus-backend /app/argus-backend
COPY --from=builder /app/config.json              /app/config.json

EXPOSE 7024
CMD ["./argus-backend"]
