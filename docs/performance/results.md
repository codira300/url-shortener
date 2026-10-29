# Performance results

Method and reproduction steps are in
[benchmark methodology](benchmark-methodology.md). Results are split into the
current post-change measurement and the archived pre-Lua baseline. The archived
numbers are historical context, not a current capacity claim.

## Current measurement (2026-10-29)

This run used the current atomic Lua rate limiter and the dependency/process
metrics. On the same 2-core / 4-thread Intel i5-7300U with 8 GB RAM and a
co-located load generator, the service sustained **4.9k-5.3k redirects per
second** once the offered rate exceeded its capacity. Every completed response
was a `302`; there were no `429`, `5xx`, Redis errors, or cache unavailability.

The 10,000 RPS target was **not** reached on this hardware. The current
ceiling is roughly 5.3k RPS with the distributed limiter enabled. The atomic Lua
script adds Redis-side work on the hot path, but the host was also not in the
same state as the earlier baseline: a same-session comparison of the pre-Lua
binary reached 5,731 RPS versus 5,349 RPS for the current binary at the same
offered rate. Treat the archived 8.1k baseline as historical rather than a
repeatable current result.

### Environment

| Item | Value |
| --- | --- |
| CPU | Intel Core i5-7300U, 2 cores / 4 logical processors |
| Memory | 8 GB |
| OS | Windows, PowerShell 7 |
| Build | Release, MinGW-w64 UCRT64 GCC 16.2, C++20 |
| App | `build-verify/urlshortener.exe` (clean Release build) |
| Load generator | `oha` v1.16.0, co-located on loopback |
| PostgreSQL | 18.6, local cluster on `127.0.0.1:5433` |
| Redis | 5.0.14.1, local server on `127.0.0.1:6380` |
| Rate limiting | Enabled on every redirect |

Benchmark-only environment for the app:

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

The high redirect limit removes the normal 2k RPS application ceiling for the
throughput test. The limiter still executes on every redirect: each request
performs one Lua `EVAL` (atomic `INCR` plus conditional `PEXPIRE`) and one
cache `GET`.

### Redirect throughput

Each case seeds a fresh URL, warms the cache with one request, then runs `oha`
for the fixed duration.

| Offered | Connections | Duration | Achieved | p50 | p95 | p99 | Max | Responses | 302 | Errors |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 100 | 10 | 10 s | 100.1 | 2.2 ms | 5.1 ms | 7.9 ms | 11.2 ms | 999 | 999 | 0 |
| 500 | 20 | 10 s | 499.3 | 2.0 ms | 6.8 ms | 11.7 ms | 48.3 ms | 4,993 | 4,993 | 0 |
| 1,000 | 50 | 10 s | 999.6 | 2.3 ms | 12.1 ms | 19.8 ms | 36.8 ms | 9,995 | 9,995 | 0 |
| 2,500 | 100 | 10 s | 2,493.2 | 4.7 ms | 22.6 ms | 35.2 ms | 77.0 ms | 24,974 | 24,974 | 0 |
| 5,000 | 100 | 15 s | 4,879.6 | 18.2 ms | 36.6 ms | 69.8 ms | 147.6 ms | 73,196 | 73,196 | 0 |
| 7,500 | 100 | 15 s | 5,026.4 | 18.5 ms | 33.9 ms | 49.0 ms | 118.0 ms | 75,320 | 75,320 | 0 |
| 10,000 | 100 | 15 s | **5,349.1** | 17.4 ms | 31.3 ms | 49.9 ms | 101.9 ms | 80,186 | 80,186 | 0 |

`oha` reports `aborted due to deadline` for a small number of iterations at
higher offered rates (2, 1, 5, 19, 71, 99, and 91 across the rows above). The
achieved rate is calculated from completed responses, not offered iterations.

At the end of the sweep:

- 269,954 redirects completed with status `302`.
- `status_429`: 0; `redis_errors_total`: 0; `redis_circuit_open_total`: 0.
- `cache_unavailable_total`: 0.
- `rate_limit_rejected_total{scope="redirect"}`: 0.
- 65 Redis client connections with `REDIS_POOL_SIZE=64` (64 pooled connections
  plus the `redis-cli` sampling session), confirming pool reuse rather than a
  new connection per request.
- Process CPU over the sweep: 78.0 seconds; resident set size: 15.6 MB.

Rows are in `benchmarks/results/oha-summary.csv`; raw JSON and logs are beside
it. A single 15-second run is one data point, not a capacity guarantee.

## Archived pre-Lua baseline (2026-10-28)

The table below was captured on 2026-10-28, before the rate limiter changed from
separate `INCR`/`PEXPIRE` calls to one atomic Lua `EVAL`. It is retained as
historical context. The raw rows are in
`benchmarks/results/oha-summary-20261028-prelua.csv`.

On that run the release build sustained **about 8.1k redirects per second** with
distributed rate limiting enabled and zero request or Redis errors. Every
response was a `302`. A limiter-bypassed control reached **9,990 RPS**, which
established the HTTP/cache ceiling on that machine at that time.

| Offered | Connections | Duration | Achieved | p50 | p95 | p99 | Max | Responses | 302 |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 100 | 10 | 10 s | 100.1 | 1.2 ms | 3.3 ms | 4.6 ms | 19.5 ms | 1,000 | 1,000 |
| 500 | 20 | 10 s | 499.3 | 1.9 ms | 5.4 ms | 9.6 ms | 40.3 ms | 4,995 | 4,995 |
| 1,000 | 50 | 10 s | 998.8 | 2.0 ms | 9.3 ms | 15.4 ms | 39.2 ms | 9,993 | 9,993 |
| 2,500 | 100 | 10 s | 2,498.1 | 5.5 ms | 19.5 ms | 27.4 ms | 50.1 ms | 24,999 | 24,999 |
| 5,000 | 100 | 15 s | 4,997.6 | 7.8 ms | 22.7 ms | 30.3 ms | 56.1 ms | 74,988 | 74,988 |
| 7,500 | 100 | 15 s | 7,496.5 | 11.2 ms | 18.5 ms | 26.2 ms | 93.1 ms | 112,417 | 112,417 |
| 10,000 | 100 | 15 s | **8,077.4** | 11.5 ms | 18.0 ms | 23.7 ms | 102.0 ms | 121,120 | 121,120 |

Baseline tuning experiments:

| Experiment | Result | Conclusion |
| --- | --- | --- |
| 64 workers, 64 Redis connections | 7,881-8,077 RPS | Best configuration on that box |
| 128 workers, 128 Redis connections | 7,486 RPS | More context switching, no gain |
| 64 workers, 300 HTTP connections | 7,613 RPS | More queued connections do not add throughput |
| Rate limiter bypassed (control) | 9,990 RPS | HTTP/cache ceiling, not a database limit |

## Redis connection-churn finding

An earlier version used `redis.pipeline()` to combine the rate-limit check and
cache lookup. Under load this opened roughly one Redis connection per request.
A standalone reproduction showed:

- 1,000 plain `SET`/`GET` pairs: 2 Redis connections.
- 1,000 pipeline executions: 1,002 Redis connections.

Windows ephemeral ports were exhausted (`TIME_WAIT` near 16,432 against a
16,384-port dynamic range). That caused `redis_errors_total` spikes, circuit
breaker flapping, and invalid benchmark numbers. The fix was to remove pipelines
from the hot path. The current limiter uses one pooled `EVAL` (atomic `INCR` and
conditional `PEXPIRE`), and the cache lookup uses one pooled `GET`; neither
creates a pipeline or a fresh connection. A plain `EVAL` probe reused
connections correctly, and the fixed-window counter remains cheaper than a
token-bucket script because it stores one integer per client and window.

Operational implication: the service can run with a small Redis pool
(16-64) without per-request connection bursts. However, repeated high-rate load
tests can still leave many sockets in `TIME_WAIT`; wait for them to drain between
runs or use a larger dynamic port range on the load-generator host.

## Reproducing

```powershell
# Start the release app with the benchmark settings above, then:
.\scripts\benchmark-oha.ps1 -Rate 10000 -Connections 100 -Duration 15s
```

For a sweep:

```powershell
$cases = @(
  @{r=100;c=10;d='10s'}, @{r=500;c=20;d='10s'},
  @{r=1000;c=50;d='10s'}, @{r=2500;c=100;d='10s'},
  @{r=5000;c=100;d='15s'}, @{r=7500;c=100;d='15s'},
  @{r=10000;c=100;d='15s'}
)
foreach ($x in $cases) {
  .\scripts\benchmark-oha.ps1 -Rate $x.r -Connections $x.c -Duration $x.d
}
```

The k6 scenarios in `benchmarks/k6/` are still useful for mixed traffic and
rate-limit behavior. Prefer them at moderate rates; k6 can generate a large
number of short-lived connections at high VU counts, which is exactly the
pattern that triggered the port issue above.

## Caveats

- The current sweep and the archived baseline were taken on different host
  states; do not read the difference as a pure code regression.
- The load generator, app, PostgreSQL, Redis, and the OS share four hardware
  threads. This is a pessimistic single-box measurement.
- Results are loopback HTTP, not a real NIC or TLS termination.
- The dataset is a single hot cache entry. Cold-cache and write-heavy workloads
  have different behavior; the workload scripts are described in the
  methodology but no cold or high-cardinality result is claimed here.
- `oha` is not bundled with the project; place it at `tools/oha/oha.exe` or pass
  `-OhaPath`.
- The 10k target is a design goal, not a measured production claim. Multi-core
  hosts and a separate load generator would change the ceiling.
