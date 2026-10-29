FROM ubuntu:24.04 AS build

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        build-essential \
        ca-certificates \
        cmake \
        libboost-url-dev \
        libhiredis-dev \
        libpq-dev \
        libredis++-dev \
        libssl-dev \
        nlohmann-json3-dev \
        pkg-config \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY CMakeLists.txt ./
COPY src ./src

RUN cmake -S . -B build \
        -DCMAKE_BUILD_TYPE=Release \
        -DURLSHORT_BUILD_TESTS=OFF \
    && cmake --build build --parallel "$(nproc)" \
    && install -Dm755 build/urlshortener /out/urlshortener

FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        ca-certificates \
        curl \
        libboost-url1.83.0 \
        libhiredis1.1.0 \
        libpq5 \
        libredis++1 \
        libssl3t64 \
    && rm -rf /var/lib/apt/lists/*

RUN useradd --system --uid 10001 --no-create-home --shell /usr/sbin/nologin urlshort

COPY --from=build /out/urlshortener /usr/local/bin/urlshortener

USER 10001
EXPOSE 8080
STOPSIGNAL SIGTERM
HEALTHCHECK --interval=30s --timeout=3s --start-period=10s --retries=3 \
    CMD ["curl", "-fsS", "http://127.0.0.1:8080/health/ready"]

ENTRYPOINT ["urlshortener"]
