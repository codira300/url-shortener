FROM ubuntu:24.04 AS build

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        build-essential \
        ca-certificates \
        cmake \
        curl \
        libboost-url-dev \
        libhiredis-dev \
        libpq-dev \
        libssl-dev \
        nlohmann-json3-dev \
        pkg-config \
    && rm -rf /var/lib/apt/lists/*

ARG REDIS_PLUS_PLUS_VERSION=1.3.15
RUN curl --fail --silent --show-error --location \
        "https://github.com/sewenew/redis-plus-plus/archive/refs/tags/${REDIS_PLUS_PLUS_VERSION}.tar.gz" \
        --output /tmp/redis-plus-plus.tar.gz \
    && tar --extract --gzip --file /tmp/redis-plus-plus.tar.gz --directory /tmp \
    && cmake -S "/tmp/redis-plus-plus-${REDIS_PLUS_PLUS_VERSION}" -B /tmp/redis-plus-plus-build \
        -DCMAKE_BUILD_TYPE=Release \
        -DREDIS_PLUS_PLUS_BUILD_TEST=OFF \
        -DREDIS_PLUS_PLUS_BUILD_STATIC=OFF \
    && cmake --build /tmp/redis-plus-plus-build --parallel "$(nproc)" \
    && cmake --install /tmp/redis-plus-plus-build \
    && rm -rf /tmp/redis-plus-plus*

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
        libssl3t64 \
    && rm -rf /var/lib/apt/lists/*

RUN useradd --system --uid 10001 --no-create-home --shell /usr/sbin/nologin urlshort

COPY --from=build /usr/local/lib/ /usr/local/lib/
RUN ldconfig

COPY --from=build /out/urlshortener /usr/local/bin/urlshortener

USER 10001
EXPOSE 8080
STOPSIGNAL SIGTERM
HEALTHCHECK --interval=30s --timeout=3s --start-period=10s --retries=3 \
    CMD ["curl", "-fsS", "http://127.0.0.1:8080/health/ready"]

ENTRYPOINT ["urlshortener"]
