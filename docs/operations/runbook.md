# Operations

## Deployment checklist

1. Apply `migrations/001_init.sql` to PostgreSQL.
2. Provision Redis. Persistence is optional for cache data, but rate-limit
   counters are part of request policy; align Redis persistence and eviction with
   your tolerance for resetting limits.
3. Set `PG_DSN`, `REDIS_HOST`, `REDIS_PORT`, and `PUBLIC_BASE_URL`.
4. Size `WORKER_THREADS`, `PG_POOL_SIZE`, and `REDIS_POOL_SIZE` together. The
   starting point is 16/16/16 on small hosts; the benchmark used 64/16/64.
5. Terminate TLS at a proxy or load balancer. The service itself speaks HTTP.
6. If a proxy is used, set `TRUST_PROXY=true` and restrict
   `TRUSTED_PROXY_CIDRS` to the proxy networks. Otherwise rate limiting keys on
   the proxy address and all clients share one bucket.
7. Protect `GET`/`DELETE` on `/api/v1/urls/{code}`. The service has no
   authentication.
8. Point readiness checks at `/health/ready`, not `/health/live`.

Docker Compose is intended for local evaluation. For production, run the binary
or image with an external PostgreSQL and Redis, use a read-only root filesystem
where possible, and keep the container user unprivileged (uid 10001 in the
provided image).

## Health and readiness

- `/health/live` returns `200` while the process is serving. Use it for
  liveness. It does not touch dependencies, so a dependency outage does not
  trigger a restart loop.
- `/health/ready` returns `200` only when PostgreSQL and Redis are both ready.
  If PostgreSQL is up and Redis is down, it returns `503` with
  `"degraded": true`. In that state redirects still work by falling back to
  PostgreSQL; creates and admin calls return `503` because their limiter is
  fail-closed.
- Readiness is cached for `READINESS_REFRESH_MS` (default 1 s) to avoid probing
  dependencies on every check.

## Logs

Logs are structured JSON on stdout/stderr. Important events:

| Event | Meaning |
| --- | --- |
| `application_configured` | Effective non-secret configuration |
| `http_listening` | Listener bound |
| `request_failed` | Unhandled exception in a handler |
| `redis_client_init_failed` | Redis client could not be created |
| `redis_circuit_open` / `redis_circuit_closed` | Breaker transition |
| `short_code_collision` | Generation retry |
| `short_code_allocation_exhausted` | All retries collided |
| `unsafe_redirect_target` | Stored URL contained CR/LF |
| `cache_invalidation_failed` | Tombstone write failed |
| `cache_create_fill_prevented_by_tombstone` | A create-time cache fill was refused |
| `thread_pool_shutdown_discarded_tasks` | Queued work exceeded the shutdown grace |
| `access` | Per-request method, target, status, duration, route, peer IP |

`LOG_LEVEL` controls verbosity. `LOG_ACCESS` enables the `access` event; it adds
serialization and I/O on the hot path, so keep it off for throughput tests.

## Metrics

Scrape `/metrics` in Prometheus format.

Key signals:

| Metric | Use |
| --- | --- |
| `urlshort_http_requests_total{route,status}` | Traffic and error ratio |
| `urlshort_http_request_duration_seconds_bucket{route}` | p50/p95/p99 per route |
| `urlshort_cache_hits_total` / `misses_total` | Cache effectiveness |
| `urlshort_cache_unavailable_total` | Redis failures seen by the cache |
| `urlshort_rate_limit_rejected_total{scope}` | Throttling by scope |
| `urlshort_rate_limit_unavailable_total{scope}` | Limiter backend failures |
| `urlshort_redis_errors_total` | Redis command failures |
| `urlshort_redis_command_duration_seconds_bucket` | Redis latency by command |
| `urlshort_redis_circuit_open_total` | Calls short-circuited by the breaker |
| `urlshort_db_pool_acquire_timeouts_total` | Pool saturation |
| `urlshort_db_pool_wait_seconds_bucket` | Time waiting for a pooled connection |
| `urlshort_db_query_duration_seconds_bucket` | PostgreSQL latency by operation |
| `urlshort_db_errors_total` | PostgreSQL failures |
| `urlshort_cache_tombstone_prevented_total` | Stale fill blocked by a tombstone |
| `urlshort_process_cpu_seconds_total` | Process CPU consumption |
| `urlshort_process_resident_memory_bytes` | Process RSS |
| `urlshort_process_threads` | Process thread count |
| `urlshort_worker_queue_depth` | Handler backlog |
| `urlshort_request_queue_rejections_total` | Overload shedding |
| `urlshort_active_requests` / `active_connections` | Concurrency |
| `urlshort_ready` | Readiness gauge |
| `urlshort_singleflight_joined_total` | Cache-miss coalescing |
| `urlshort_code_collisions_total` | Short-code pressure |

Suggested alerts:

- `ready == 0` for more than 30 s.
- `rate(http_requests_total{status=~"5.."}[5m])` above 1% of total.
- `rate(redis_errors_total[5m]) > 0` or `redis_circuit_open_total` increasing.
- `worker_queue_depth` sustained above 50% of capacity.
- `request_queue_rejections_total` increasing.
- p99 redirect latency above the SLO for 10 minutes.
- `db_pool_acquire_timeouts_total` increasing.

## Sizing guidance

The app is I/O-bound, but every redirect does a Redis round trip and every
worker blocked on Redis or PostgreSQL is unavailable for other requests.

- Start with `WORKER_THREADS` at 2-4x the CPU cores for I/O-heavy redirect
  traffic, then measure.
- Keep `REDIS_POOL_SIZE` and `PG_POOL_SIZE` close to the number of workers that
  can be inside those calls at once. Making the pools much smaller than the
  worker count creates avoidable acquire timeouts.
- Keep `REDIS_POOL_WAIT_TIMEOUT_MS` and `PG_ACQUIRE_TIMEOUT_MS` much shorter
  than the HTTP read/write timeouts so saturation fails fast.
- The benchmark used 64 workers and 64 Redis connections; 128 workers was
  slower on a four-thread host.

## Runbooks

### Redis is down or slow

Symptoms: `cache_unavailable_total`, `redis_errors_total`, and possibly
`redis_circuit_open_total` rise; readiness shows `"redis": "down"`.

Expected behavior: redirects fail open and hit PostgreSQL. Creates and admin
calls return `503` because the limiter is fail-closed. The breaker opens and
stops issuing commands until the cooldown expires.

Actions:

1. Check Redis availability, latency, maxmemory, and eviction pressure.
2. Watch PostgreSQL load. A Redis outage turns every redirect into a database
   read, and the single-flight group limits but does not eliminate the load.
3. If the outage will be long, scale the app down or shed redirect traffic at
   the edge; do not let the database become the next outage.
4. Restore Redis. The breaker probes and closes automatically.

### PostgreSQL is down

Symptoms: `db_errors_total`, `db_pool_acquire_timeouts_total`, readiness
`"postgres": "down"`; redirects return `503` once the cache misses.

Expected behavior: cached redirects continue to work because the cache is
consulted first. Cache misses cannot be served. Creates and metadata always
need PostgreSQL.

Actions: restore connectivity, credentials, or capacity. Do not restart the app
unless its process is unhealthy; it recovers when the database does.

### High p99 latency without errors

Check `worker_queue_depth`, `db_pool_in_use`, Redis latency, and CPU. A common
cause is a pool smaller than the in-flight concurrency. Another is a hot code
whose cache entry is absent or negative. Use the route label to separate
redirects from creates.

### Rate limiting rejects legitimate traffic

429s are expected above the configured window. Check the scope and client
identity. If clients sit behind a proxy, enable `TRUST_PROXY` with correct CIDRs;
otherwise the proxy address is the identity. Raise `*_RATE_LIMIT_RPS` and
`*_RATE_LIMIT_BURST` together, and remember the window length is
`BURST / RPS` seconds.

### Ephemeral port exhaustion on the load-generator host

Symptoms during benchmarks: large `TIME_WAIT` counts, Redis connection errors,
or the load generator failing to connect. The application's Redis client reuses
pooled connections, but rapid high-rate tests can still exhaust a small Windows
dynamic port range. Wait for `TIME_WAIT` to drain, use fewer load-generator
connections, or widen the port range with administrator rights. Do not
compensate by adding client-side pipelines or a new connection per request.

### Graceful shutdown

The process handles `SIGTERM`/Ctrl+C: it stops accepting new connections, drains
the HTTP server and worker queue for `SHUTDOWN_GRACE_MS`, stops health probing,
and exits. Send `SIGKILL` only after the grace period. In Kubernetes, set
`terminationGracePeriodSeconds` above `SHUTDOWN_GRACE_MS`.

## Backups and data retention

- PostgreSQL is the durable store. Back it up with your normal PITR strategy.
- Redis holds cache entries and rate-limit counters. Losing it is recoverable:
  caches refill from PostgreSQL and limits reset. Enable Redis persistence only
  if resetting limits or cache warmth is unacceptable.
- Expired and deactivated rows remain in `urls`. Add a retention job if the
  table grows beyond your needs; the read path already treats them as gone.
- Cache keys are disposable; do not back up Redis as a system of record.

## Security

- No authentication is built in. Restrict administration endpoints at the edge.
- Keep `TRUST_PROXY` off unless a trusted proxy strips client-supplied
  `X-Forwarded-For`.
- The URL validator allows only `http` and `https`; it does not fetch the
  destination. Still treat user-supplied URLs as untrusted content.
- Store database credentials in a secret manager, not in the image. The DSN is
  redacted in startup logs but is still a secret.
- The container image runs as uid 10001 and installs only runtime libraries and
  curl for health checks.
