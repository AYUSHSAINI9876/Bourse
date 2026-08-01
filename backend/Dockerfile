# syntax=docker/dockerfile:1
#
# Multi-stage build. The runtime image carries no compiler, no build tree and
# no package manager -- roughly 80 MB against ~1.2 GB for a single-stage build.
#
# IMPORTANT: the build context is the *repository root*, not backend/. The
# server embeds frontend/index.html, so the context has to span both halves:
#
#   docker build -f backend/Dockerfile .          <-- correct
#   docker build backend/                         <-- fails at the embed step
#
# docker-compose.yml and render.yaml both set this correctly.

# ---------------------------------------------------------------------------
# Stage 1: build
# ---------------------------------------------------------------------------
FROM gcc:15 AS build

RUN apt-get update \
 && apt-get install -y --no-install-recommends cmake ninja-build \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /src

# The layout under /src mirrors the repository so the relative path from
# backend/CMakeLists.txt to ../frontend/index.html resolves unchanged.
#
# Build definition first, so configuration stays cached independently of
# source edits.
COPY backend/CMakeLists.txt ./backend/
COPY backend/cmake/ ./backend/cmake/

COPY backend/include/ ./backend/include/
COPY backend/src/ ./backend/src/
COPY backend/apps/ ./backend/apps/
# Required: cmake/EmbedAsset.cmake compiles this into the binary, so a missing
# frontend fails the build rather than silently producing a server with no
# dashboard. Only the HTML is needed -- not the deploy scripts beside it.
COPY frontend/index.html ./frontend/index.html

# Tests need GoogleTest, which is not in this image; the release artefact does
# not need them either. CI runs the suite separately.
RUN cmake -B build -S backend -G Ninja \
      -DCMAKE_BUILD_TYPE=Release \
      -DBOURSE_BUILD_TESTS=OFF \
 && cmake --build build --parallel

# ---------------------------------------------------------------------------
# Stage 2: runtime
# ---------------------------------------------------------------------------
FROM debian:trixie-slim AS runtime

RUN apt-get update \
 && apt-get install -y --no-install-recommends libstdc++6 \
 && rm -rf /var/lib/apt/lists/* \
 && useradd --system --create-home --shell /usr/sbin/nologin bourse

COPY --from=build /src/build/bin/bourse-server /usr/local/bin/bourse-server

USER bourse
WORKDIR /home/bourse

# 6380 speaks RESP, 8080 serves the REST API and the dashboard. Container
# platforms that publish a single port want the HTTP one; those that read
# $PORT override 8080 at run time without a rebuild.
EXPOSE 6380 8080

# Exec form so bourse-server is PID 1 and receives SIGTERM directly, which is
# what makes `docker stop` a graceful shutdown rather than a 10-second kill.
ENTRYPOINT ["/usr/local/bin/bourse-server"]
CMD ["--host", "0.0.0.0", "--port", "6380"]
