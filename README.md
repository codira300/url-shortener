# URL Shortener

A production-inspired URL shortener written in C++20. It uses PostgreSQL as the
durable source of truth and Redis for hot-path caching and distributed
fixed-window rate limiting. The HTTP layer is Boost.Beast/Asio with a bounded
worker pool, explicit overload behavior, structured logs, and Prometheus
metrics.

The hot redirect path is designed to avoid database access on cache hits while
remaining safe when Redis fails: redirects fail open, creates fail closed, and
an in-process circuit breaker prevents a sick Redis from stalling every worker.

## Measured performance

Current post-change sweep on a 2-core / 4-thread Intel i5-7300U, 8 GB RAM,
Windows, with the release binary, the load generator co-located, and production
rate limiting enabled (atomic Lua limiter):

| Offered load | Achieved | p50 | p95 | p99 | Responses | Errors |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 100 RPS | 100.1 | 2.2 ms | 5.1 ms | 7.9 ms | 999 | 0 |
| 500 RPS | 499.3 | 2.0 ms | 6.8 ms | 11.7 ms | 4,993 | 0 |
| 1,000 RPS | 999.6 | 2.3 ms | 12.1 ms | 19.8 ms | 9,995 | 0 |
| 2,500 RPS | 2,493.2 | 4.7 ms | 22.6 ms | 35.2 ms | 24,974 | 0 |
| 5,000 RPS | 4,879.6 | 18.2 ms | 36.6 ms | 69.8 ms | 73,196 | 0 |
| 7,500 RPS | 5,026.4 | 18.5 ms | 33.9 ms | 49.0 ms | 75,320 | 0 |
| 10,000 RPS | **5,349.1** | 17.4 ms | 31.3 ms | 49.9 ms | 80,186 | 0 |

All completed responses were `302`; Redis reported zero errors, zero circuit
opens, and zero cache-unavailable events. The 10,000 RPS target was not reached
on this four-thread development machine: the current ceiling is about 5.3k RPS
with the distributed limiter enabled. The archived pre-Lua baseline reached
8.1k RPS, so the historical numbers are retained for comparison, not presented
as a current result. See
[docs/performance/results.md](docs/performance/results.md) for methodology and
raw data.

## Quick start

### Docker Compose

```bash
docker compose up --build
curl -s http://localhost:8080/health/ready
```

The schema is applied automatically on first start by the Postgres container.
Postgres and Redis data are kept in named volumes; if you reuse an existing
volume, apply new migrations manually.

### Local build (Linux/macOS)

Dependencies: CMake 3.25+, a C++20 compiler, Boost 1.83+ (`boost-url`),
OpenSSL, libpq, hiredis, redis-plus-plus, nlohmann-json, pkg-config, and Catch2 3
when tests are enabled.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
./build/urlshortener
```

### Local build (Windows, MinGW-w64 UCRT64)

The repository was developed with MSYS2 UCRT64. Adjust paths as needed:

```powershell
cmake -S . -B build -G "MinGW Makefiles" `
  -DCMAKE_C_COMPILER=C:/msys64/ucrt64/bin/gcc.exe `
  -DCMAKE_CXX_COMPILER=C:/msys64/ucrt64/bin/g++.exe `
  -DCMAKE_BUILD_TYPE=Release
cmake --build build -j 4
ctest --test-dir build --output-on-failure
```

Apply the schema before starting the service:

```powershell
psql "postgresql://urlshortener:urlshortener_dev@127.0.0.1:5433/urlshortener" `
  -f migrations/001_init.sql
Copy-Item .env.example .env
```

The application reads process environment variables; `.env` is a reference
file, not loaded automatically. Export variables with your shell, container
runtime, or service manager.

## API

### Create a short URL

```http
POST /api/v1/urls
Content-Type: application/json

{"url":"https://example.com/some/long/path","expires_at":null}
```

`expires_at` is optional and must be an RFC 3339 timestamp with a timezone, in
the future.

```json
HTTP/1.1 201 Created
Location: http://localhost:8080/Ab3xY9z

{
  "short_code": "Ab3xY9z",
  "short_url": "http://localhost:8080/Ab3xY9z",
  "original_url": "https://example.com/some/long/path",
  "created_at": "2026-10-28T02:30:00Z",
  "expires_at": null,
  "is_active": true
}
```

### Redirect

```http
GET /{short_code}
```

Returns `302 Found` with `Location`, `Cache-Control: no-store`, and
`Referrer-Policy: no-referrer`. Expired or deactivated codes return `404`.

### Read metadata

```http
GET /api/v1/urls/{short_code}
```

Returns the same representation as create. Inactive or expired codes return
`410 Gone`.

### Deactivate

```http
DELETE /api/v1/urls/{short_code}
```

Returns `204 No Content` on success. The cache entry is replaced with a
short-lived tombstone so a racing cache fill cannot resurrect the URL.

### Health and metrics

- `GET /health/live` - process liveness, always `200` while serving.
- `GET /health/ready` - checks Postgres and Redis; `200` when ready, `503`
  otherwise. Redis-only failure is reported as `degraded`.
- `GET /metrics` - Prometheus text exposition.

Errors use a consistent envelope:

```json
{"error":{"code":"rate_limited","message":"too many requests"}}
```

## Configuration

All settings are environment variables. See [.env.example](.env.example) for
the full list and defaults. Frequently changed values:

| Variable | Default | Purpose |
| --- | --- | --- |
| `HTTP_HOST` / `HTTP_PORT` | `127.0.0.1` / `8080` | Listener address |
| `HTTP_IO_THREADS` | `4` | Asio I/O threads |
| `WORKER_THREADS` | `16` | Request handler threads |
| `WORKER_QUEUE_CAPACITY` | `4096` | Bounded handler queue |
| `PG_DSN` | local dev DSN | PostgreSQL connection string |
| `PG_POOL_SIZE` | `16` | PostgreSQL connection pool |
| `REDIS_HOST` / `REDIS_PORT` | `127.0.0.1` / `6380` | Redis endpoint |
| `REDIS_POOL_SIZE` | `16` | Redis connection pool |
| `CACHE_TTL_SECONDS` | `300` | Positive cache TTL |
| `REDIRECT_RATE_LIMIT_RPS` | `2000` | Redirect refill rate |
| `REDIRECT_RATE_LIMIT_BURST` | `4000` | Redirect window capacity |
| `RATE_LIMIT_FAIL_OPEN_REDIRECT` | `true` | Allow redirects if Redis is down |
| `PUBLIC_BASE_URL` | `http://localhost:8080` | Prefix in API responses |
| `TRUST_PROXY` | `false` | Trust `X-Forwarded-For` from trusted CIDRs |

### Rate limiter semantics

The limiter is a distributed fixed-window counter:

```
window_ms = ceil(BURST * 1000 / RPS)
key       = rl:v2:<scope>:<client-identity>:<window-id>
```

Each request performs one atomic Lua `EVAL` that executes `INCR` and, on the
first increment in a window, `PEXPIRE` for two windows. The counter is kept for
two windows. `429` responses include `Retry-After`. The fixed window is
intentionally simple and connection-reuse safe; the previous Redis pipeline
approach exhausted ephemeral ports under load (see
[ADR 0003](docs/architecture/decision-records/0003-fixed-window-rate-limiter.md)
and
[ADR 0005](docs/architecture/decision-records/0005-atomic-rate-limit-increment.md)).

## Design highlights

- **Read path:** Redis `GET` on `url:v1:<code>`, then a database lookup through a
  per-code single-flight group on a miss. Hit responses never touch Postgres.
- **Cache safety:** positive entries, negative entries, and delete tombstones
  are tagged. Lua scripts prevent a slow database read from overwriting a
  tombstone or negative entry.
- **Backpressure:** the HTTP layer admits work into a bounded queue; when it is
  full the request is rejected with `503` and `Retry-After` rather than growing
  memory without bound.
- **Failure policy:** redirects fail open (cache/database fallback), creates
  fail closed. A circuit breaker stops hammering Redis after repeated failures
  and probes it again after a cooldown.
- **Observability:** structured JSON logs, request histograms per route, cache
  hit/miss/negative/unavailable counters, limiter decisions, Redis circuit
  state, database pool metrics, and active request/connection gauges.

Read [docs/architecture/system-overview.md](docs/architecture/system-overview.md)
for the full component view, then the focused architecture documents.

## Testing

```bash
# Unit tests only
ctest --test-dir build -R unit --output-on-failure

# Integration and concurrency tests need PostgreSQL, Redis, and the schema
cmake -S . -B build -DURLSHORT_ENABLE_INTEGRATION_TESTS=ON
cmake --build build --parallel
PG_DSN=postgresql://urlshortener:urlshortener_dev@127.0.0.1:5432/urlshortener \
REDIS_HOST=127.0.0.1 REDIS_PORT=6379 \
ctest --test-dir build --output-on-failure
```

Tests skip (exit code 4) when their backends are unavailable.

## Benchmarking

Two harnesses are included:

- `scripts/benchmark.ps1` - k6 scenarios for redirect, mixed, and rate-limit
  behavior. Use modest VU counts; the defaults are deliberately bounded.
- `scripts/benchmark-oha.ps1` - `oha` runner for high-throughput measurements.
  It expects `tools/oha/oha.exe` or `-OhaPath`.
- `scripts/benchmark-workloads.ps1` - hot, cold-cache, high-cardinality, and
  missing-code workloads with a separate CSV summary.

```powershell
# High-throughput redirect run
.\scripts\benchmark-oha.ps1 -Rate 10000 -Connections 100 -Duration 15s

# k6 mixed workload
.\scripts\benchmark.ps1 -Mode mixed -Rate 500 -Duration 20s
```

Benchmark results are written to `benchmarks/results/`. The measured results
above were produced with the release binary and these benchmark-only settings:

```text
WORKER_THREADS=64
REDIS_POOL_SIZE=64
REDIS_CONNECT_TIMEOUT_MS=2000
REDIS_SOCKET_TIMEOUT_MS=5000
REDIS_POOL_WAIT_TIMEOUT_MS=5000
REDIS_CIRCUIT_FAILURE_THRESHOLD=1000
REDIRECT_RATE_LIMIT_RPS=1000000
REDIRECT_RATE_LIMIT_BURST=2000000
```

These limits only remove the artificial 2k RPS ceiling for the benchmark; the
rate limiter still executes on every redirect.

## Project layout

```
src/
  app/            request routing, URL lifecycle, rate-limit policy
  cache/          Redis gateway, circuit breaker, URL cache codec
  config/         environment parsing and validation
  db/             libpq pool and PostgreSQL repository
  domain/         short-code generation, URL validation, records
  http/           Boost.Beast server, router, bounded worker admission
  net/            trusted-proxy aware client identity
  observability/  Prometheus metrics and readiness checks
  ratelimit/      distributed fixed-window limiter
  util/           logging, time, single-flight, thread pool
tests/            unit, integration, concurrency suites
benchmarks/       k6 scenarios and recorded results
migrations/       PostgreSQL schema
scripts/          benchmark runners
docs/             architecture, operations, research, ADRs
```

## Limitations and operational notes

- There is no authentication or tenant model. Metadata and delete endpoints
  must be protected at the edge (API gateway, mTLS, or private network).
- The service is stateless; run multiple replicas behind a load balancer. Redis
  and PostgreSQL are shared dependencies.
- Expiration is enforced on read and during cache fill; there is no background
  reaper. Expired rows remain in PostgreSQL.
- Rate limiting is per client identity and per fixed window. Proxies must be
  configured with `TRUST_PROXY` and `TRUSTED_PROXY_CIDRS`; otherwise all clients
  behind a proxy share the proxy's identity.
- The measured 5.3k RPS is a single-node, co-located-generator result, not a
  claim about a production cluster. See
  [docs/performance/results.md](docs/performance/results.md).
- Docker images were authored for Linux/amd64 and are not exercised in this
  Windows development environment; CI builds and tests the native Linux binary.

## Documentation

- [Documentation index](docs/README.md)
- [System overview](docs/architecture/system-overview.md)
- [Request lifecycle](docs/architecture/request-lifecycle.md)
- [Caching and consistency](docs/architecture/caching-and-consistency.md)
- [Performance methodology](docs/performance/benchmark-methodology.md)
- [Performance results](docs/performance/results.md)
- [Operations runbook](docs/operations/runbook.md)
- [Repository review](docs/research/repository-review.md)
- [Research notes](docs/research/architecture-decisions.md)
- [Learning guide](docs/learning-guide.md)
- [Architecture decision records](docs/architecture/decision-records/)
