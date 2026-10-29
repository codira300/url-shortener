# Benchmark methodology

The goal is to report measured behavior, not a design target. Every result in
[results](results.md) names the hardware, build, workload, and settings used.

## Harnesses

- `scripts/benchmark-oha.ps1` - high-throughput redirects with `oha`. It creates
  a URL, warms the cache, runs a fixed-duration load, checks readiness, and
  appends a row to `benchmarks/results/oha-summary.csv`.
- `scripts/benchmark.ps1` - k6 scenarios for redirects, mixed create/redirect
  traffic, and rate-limit behavior. Use `-Mode redirect|mixed|rate-limit`.
- `scripts/benchmark-workloads.ps1` - `oha` workloads for hot, cold-cache,
  high-cardinality, and missing-code traffic. Results go to
  `benchmarks/results/workloads-summary.csv`.
- `benchmarks/k6/mixed.js` - percentage of creates mixed into redirect traffic.
- `benchmarks/k6/rate-limit.js` - expects `302` or `429`, validating that limits
  reject rather than error.

The k6 runner uses modest VU counts by default. High VU counts create many
short-lived connections and can exhaust the load generator's ephemeral ports on
Windows; that is a generator problem, not a service result.

## Redirect throughput procedure

1. Start PostgreSQL and Redis. Apply `migrations/001_init.sql`.
2. Build a Release binary.
3. Start the service with the benchmark-only settings:

   ```text
   WORKER_THREADS=64
   REDIS_POOL_SIZE=64
   REDIS_CONNECT_TIMEOUT_MS=2000
   REDIS_SOCKET_TIMEOUT_MS=5000
   REDIS_POOL_WAIT_TIMEOUT_MS=5000
   REDIS_CIRCUIT_FAILURE_THRESHOLD=1000
   REDIS_CIRCUIT_COOLDOWN_MS=1000
   REDIRECT_RATE_LIMIT_RPS=1000000
   REDIRECT_RATE_LIMIT_BURST=2000000
   ```

   The high limits remove the normal application ceiling so the HTTP, cache, and
   Redis path can be measured. The limiter still executes on every redirect.

4. Run one case:

   ```powershell
   .\scripts\benchmark-oha.ps1 -Rate 10000 -Connections 100 -Duration 15s
   ```

5. Run a sweep only when the host is otherwise idle. Wait for `TIME_WAIT` to
   drain between high-rate runs.

Each case seeds a new code and warms it once. That measures a hot cache entry,
not cold-cache behavior. Report the median of multiple runs if comparing
configurations; a single run is a data point, not a capacity claim.

## What to record

- offered rate, connections, duration, achieved requests/s,
- total responses and status distribution,
- p50/p95/p99/max latency,
- aborted iterations and error distribution,
- app worker/pool settings and build type,
- `redis_errors_total`, `redis_circuit_open_total`,
  `cache_unavailable_total`, and limiter rejections,
- Redis command count and client connection count where available.

`oha`'s `aborted due to deadline` count means the generator gave up on some
iterations; report it separately from completed responses. Do not compute
success as `1 - aborted/total` without stating the method.

## Rate-limit test

Run with the normal limiter settings, not the benchmark-only high limits:

```powershell
.\scripts\benchmark.ps1 -Mode rate-limit -Rate 1000 -Duration 20s
```

Expected outcomes are `302` and `429`. A `5xx` or a connection error is a
failure. Check `Retry-After` on a sample of `429` responses and confirm that the
counter key has a TTL.

## Mixed workload

```powershell
.\scripts\benchmark.ps1 -Mode mixed -Rate 500 -Duration 20s
```

This mixes creates with redirects (5% creates by default) and is useful for
observing write amplification, code collisions, and database pool pressure. It
is not a redirect-throughput measurement.

## Cold-cache and high-cardinality workloads

The default redirect harness warms one entry. Cold-cache behavior is dominated
by PostgreSQL and single-flight, and high-cardinality behavior is dominated by
cache footprint. Use the workload harness:

```powershell
# Hot single entry
.\scripts\benchmark-workloads.ps1 -Mode hot -Rate 5000 -Duration 15s

# Seeded codes, cache keys deleted, then one cold pass and one warm pass
.\scripts\benchmark-workloads.ps1 -Mode cold -Codes 200 -Connections 10

# Seeded codes, warmed once, then run with the cache populated
.\scripts\benchmark-workloads.ps1 -Mode high-cardinality -Codes 500 -Duration 15s

# Random 8-character paths that do not exist; exercises DB-miss/negative cache
.\scripts\benchmark-workloads.ps1 -Mode missing -Rate 1000 -Duration 15s
```

Seeding uses the public create endpoint and therefore the create rate limit.
Raise the create limit for a benchmark deployment or use a small `-Codes` value;
do not bypass the limiter by writing rows directly if the goal is to measure the
service as deployed.

For cold mode, the runner deletes `url:v1:<code>` with `redis-cli` before the
first pass, then repeats the same URL file to show the warm-cache result. Cold
passes are one request per code (`-n Codes`), so the numbers are a cold-start
sample rather than a steady-state cold-miss rate. For a steady-state cold-miss
rate, rotate through more codes than the number of requests or run with a small
Redis eviction budget.

Use low concurrency for the cold pass. With duplicate in-flight misses, joiners
can exceed the 250 ms per-key single-flight wait and receive `503`; that
measures the fail-fast policy, not first-hit database latency. Completed
cold-cache, high-cardinality, and missing-code measurements are in
[results](results.md).

## Sanitizer and correctness runs

Correctness tests are separate from throughput:

```powershell
ctest --test-dir build --output-on-failure
```

CI runs the same suites under ASan/UBSan and TSan. Do not run throughput tests
against a sanitizer build and compare the numbers.

## Caveats

- A co-located generator competes for CPU, memory bandwidth, and the loopback
  stack with the service.
- Loopback HTTP is not a real NIC, TLS terminator, or production network.
- The dataset in the completed sweep is one hot cache entry.
- Results are sensitive to worker count, Redis pool size, and the host's
  `TIME_WAIT` state.
- The 10,000 RPS target is a goal, not a guarantee. Report the achieved rate.
