# Learning guide

This guide is a map for reading the codebase and reproducing the engineering
decisions. It assumes comfort with C++ and basic HTTP/database concepts.

## 1. Start with the contract

Read:

- `README.md` - API, configuration, measured behavior.
- `migrations/001_init.sql` - the only durable schema.
- `src/http/Router.cpp` - path/method matching and reserved routes.

Exercise: start the service and create, redirect, inspect, and delete a URL with
curl. Observe the `302`, `Location`, `Cache-Control`, and `404`/`410` behavior.

## 2. Follow one redirect

Read in this order:

1. `src/http/HttpServer.cpp` - Beast session, keep-alive, admission.
2. `src/app/Application.cpp` - `handle`, `routeAndHandle`, `enforceRateLimit`.
3. `src/ratelimit/RedisRateLimiter.cpp` - fixed-window counter.
4. `src/app/UrlService.cpp` - `resolve`, `loadFromDatabase`, single-flight.
5. `src/cache/RedisUrlCache.cpp` - lookup and tagged values.
6. `src/db/PostgresUrlRepository.cpp` - SQL and status mapping.

Questions to answer while reading:

- Where does a cache hit stop touching PostgreSQL?
- What happens if the cache read fails?
- Why is the rate limit checked before the cache read?
- Where is the `302` actually written?

## 3. Follow one create and one delete

Read `UrlService::create` and `UrlService::remove`.

- How is a collision detected and retried?
- What is written to Redis after a successful create?
- Why does delete write a tombstone instead of deleting the key?
- How do the Lua scripts prevent a stale fill from winning?

Exercise: create a URL, read its metadata, delete it, then request it. Confirm
that the redirect returns `404` and metadata returns `404`/`410` as documented.

## 4. Study the failure paths

Read:

- `src/cache/RedisGateway.cpp` - circuit breaker and error mapping.
- `src/db/PgPool.cpp` - bounded pool and acquire timeout.
- `src/util/ThreadPool.cpp` - bounded queue and rejection.
- `src/observability/HealthService.cpp` - readiness refresh.

Exercise: stop Redis and observe `/health/ready` (`degraded`), redirects still
working (fail-open), and creates returning `503` (fail-closed). Restart Redis
and watch the circuit close.

## 5. Study the hot path

The productive loop is: measure, find the dominant cost, change one thing,
measure again.

Key facts from the project:

- A redirect performs one Redis rate-limit `EVALSHA` and one Redis `GET` on a warm cache.
- The limiter's first request in a window executes `PEXPIRE` inside the same Lua script; later requests only increment.
- `redis-plus-plus` pipelines opened a connection per call on this platform.
  Avoid reintroducing them without a standalone connection-count test.
- More workers and more HTTP connections did not improve throughput past the
  measured ceiling.

Exercise: use `scripts/benchmark-oha.ps1` at 5k RPS. Then increase the worker
count and Redis pool to 128 and compare. Explain the result using queueing and
context-switch costs rather than assuming "more threads is faster."

## 6. Read the cache encoding

Read `src/cache/CacheCodec.cpp` and its unit tests.

- What distinguishes `P`, `N`, and `D` values?
- What happens to a malformed value?
- How is the TTL bounded by the URL's expiration?
- Why is jitter useful?

## 7. Read the concurrency tests

`tests/concurrency/test_concurrency.cpp` demonstrates:

- single-flight coalescing under a barrier,
- thread-pool draining,
- concurrent cache/limiter behavior with real backends.

Run them with backends available. Try to predict which operations are allowed
under the fixed-window limiter before reading the assertions.

## 8. Reproduce a benchmark

1. Build with `-DCMAKE_BUILD_TYPE=Release`.
2. Start PostgreSQL and Redis, apply the schema.
3. Start the app with the benchmark settings from `docs/performance/results.md`.
4. Run `scripts/benchmark-oha.ps1`.
5. Record raw JSON and the CSV row.
6. Check `redis_errors_total`, `redis_circuit_open_total`, and `TIME_WAIT`.

Do not compare runs made with different worker/pool settings or while
`TIME_WAIT` is saturated.

## 9. Suggested extensions

- Replace the fixed window with a token bucket on a dedicated per-worker Redis
  connection, then compare smoothness and throughput.
- Add authentication and per-tenant quotas.
- Add a background expiration reaper with a partial index.
- Add OpenTelemetry traces around the cache/database path.
- Add a Redis Cluster-compatible key layout (hash tags for multi-key scripts).
- Run a multi-node load test with a separate generator and measure the real
  horizontal scaling curve.

## Reading checklist

- [ ] I can trace a redirect from socket to `302`.
- [ ] I can explain why the cache is not the source of truth.
- [ ] I can explain fail-open versus fail-closed behavior.
- [ ] I can explain the pipeline connection-churn bug and its fix.
- [ ] I can reproduce the performance table and interpret its limits.
- [ ] I can identify which metrics would page an on-call engineer.

