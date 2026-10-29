# Performance results

Method and reproduction steps are in
[benchmark methodology](benchmark-methodology.md). Results are split into the
current post-change measurement and the archived pre-Lua baseline. The archived
numbers are historical context, not a current capacity claim.

## Current measurement (2026-10-29)

This run used the current atomic Lua rate limiter with cached `EVALSHA` and the
dependency/process metrics. On the same 2-core / 4-thread Intel i5-7300U with
8 GB RAM and a co-located load generator, three consecutive 10,000-offered runs
reached **7,195, 8,062, and 7,480 RPS** (median **7,480 RPS**). Every completed
response was a `302`; there were no `429`, `5xx`, Redis errors, or cache
unavailability.

The 10,000 RPS target was **not** reached on this hardware. The current ceiling
is about **7.5k-8.1k RPS** with the distributed limiter enabled. The atomic Lua
limiter adds Redis-side work on the hot path, but loading the script once and
using `EVALSHA` recovers most of the cost: the earlier `EVAL` build measured
5,349 RPS at the same offered rate. Its rows are archived in
`benchmarks/results/oha-summary-20261029-eval.csv`.

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
performs one Redis `EVALSHA` (atomic `INCR` plus conditional `PEXPIRE`) and one
cache `GET`.

### Clean high-rate runs

Each case seeds a fresh URL, warms the cache with one request, then runs `oha`
for 15 seconds at 10,000 offered RPS with 100 connections.

| Run | Achieved | p50 | p95 | p99 | Max | Responses | 302 | Aborted |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 7,194.9 | 12.7 ms | 20.9 ms | 38.3 ms | 220.5 ms | 107,896 | 107,896 | 78 |
| 2 | **8,062.3** | 11.3 ms | 18.5 ms | 30.7 ms | 80.7 ms | 120,882 | 120,882 | 91 |
| 3 | 7,479.6 | 12.3 ms | 19.4 ms | 33.9 ms | 102.9 ms | 112,317 | 112,317 | 96 |
| Median | 7,479.6 | 12.3 ms | 19.4 ms | 33.9 ms | 102.9 ms | 112,317 | 112,317 | 96 |

### Low-rate sweep

These rows came from a later sweep while the service was otherwise healthy.
Every offered rate up to 2,500 was met.

| Offered | Connections | Duration | Achieved | p50 | p95 | p99 | Max | Responses | 302 | Errors |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 100 | 10 | 10 s | 100.0 | 1.4 ms | 7.8 ms | 105.3 ms | 202.3 ms | 999 | 999 | 0 |
| 500 | 20 | 10 s | 499.3 | 1.4 ms | 5.8 ms | 20.8 ms | 37.1 ms | 4,996 | 4,996 | 0 |
| 1,000 | 50 | 10 s | 999.6 | 1.6 ms | 9.4 ms | 18.2 ms | 54.3 ms | 10,000 | 10,000 | 0 |
| 2,500 | 100 | 10 s | 2,495.1 | 2.7 ms | 17.0 ms | 40.6 ms | 165.1 ms | 24,978 | 24,978 | 0 |

The same sweep attempted 5,000, 7,500, and 10,000 offered, but an unrelated
SQL Server process on the host consumed 30-60% of total CPU during that window.
Those rows fell to 3.7k-4.1k RPS and are **not used as capacity numbers**. The
clean high-rate runs above were captured before that contention began.

At the end of the clean high-rate runs:

- 341,095 redirects completed with status `302` across the three runs.
- `status_429`: 0; `redis_errors_total`: 0; `redis_circuit_open_total`: 0.
- `cache_unavailable_total`: 0.
- `rate_limit_rejected_total{scope="redirect"}`: 0.
- Redis connection count stayed at the pool size plus the sampling client,
  confirming pool reuse rather than a new connection per request.

Rows are in `benchmarks/results/oha-summary.csv`; raw JSON is beside it. A
single 15-second run is one data point, not a capacity guarantee.

### Workload shapes

These runs used the same release build and the normal redirect path, with
`PG_POOL_SIZE=64` and `PG_ACQUIRE_TIMEOUT_MS=2000` so cold misses would not
compete for the default 16-connection pool. They were collected on the same
contended host, so read them as shape comparisons rather than clean capacity
numbers.

| Workload | URLs | Connections | Offered | Achieved | p50 | p95 | p99 | Max | Responses | Result |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| Cold cache | 200 | 10 | unpaced | 1,777.1 | 1.8 ms | 21.1 ms | 55.9 ms | 102.1 ms | 200 | 200 `302` |
| Warm after cold | 200 | 10 | unpaced | 5,404.6 | 1.5 ms | 3.5 ms | 4.5 ms | 5.4 ms | 200 | 200 `302` |
| High cardinality | 500 | 50 | 2,500 | 2,495.6 | 1.8 ms | 6.0 ms | 10.7 ms | 42.0 ms | 15,000 | 15,000 `302` |
| Missing codes | random | 50 | 1,000 | 999.6 | 1.4 ms | 9.7 ms | 16.9 ms | 37.4 ms | 10,000 | 10,000 `404` |
| Hot control | 1 | 50 | 2,500 | 2,498.4 | 2.1 ms | 12.0 ms | 23.4 ms | 56.0 ms | 15,000 | 15,000 `302` |

The cold pass sends each of 200 distinct URLs once, with low concurrency so
duplicate in-flight misses do not dominate. The immediate second pass warms the
same 200 entries and shows the cache-hit path. High cardinality cycles through
500 warmed codes; missing codes exercise DB misses and negative-cache writes
with `404`.

An initial 50-connection cold pass produced 30 `503`s. Service metrics showed no
database, Redis, queue-rejection, or rate-limit errors, which is consistent with
concurrent joiners exceeding the 250 ms per-key single-flight wait while the
database pool was still cold. That attempt is excluded. The cold-cache row above
uses 10 connections, where every response completed without an error. This is a
measured behavior of the fail-fast single-flight policy, not a capacity result.

Rows and raw JSON are in `benchmarks/results/workloads-summary.csv`.

## Why `EVALSHA`

A standalone Redis microbenchmark on the same host measured one fixed-window
script at 50 connections:

| Command | Measured throughput |
| --- | ---: |
| `EVAL` with the full script body | 17,164-17,680 commands/s |
| `EVALSHA` after `SCRIPT LOAD` | 21,228-21,455 commands/s |

Caching the SHA avoids resending and re-hashing the script on every redirect.
The application now loads each script once, executes it with `EVALSHA`, and
reloads on a `NOSCRIPT` error (for example after a Redis restart or
`SCRIPT FLUSH`). This keeps the atomic `INCR`/`PEXPIRE` semantics while reducing
Redis CPU on the hot path.

## Archived pre-Lua baseline (2026-10-28)

The table below was captured on 2026-10-28, before the rate limiter changed from
separate `INCR`/`PEXPIRE` calls to one atomic Lua script. It is retained as
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
from the hot path. The current limiter uses one pooled `EVALSHA` (atomic `INCR`
and conditional `PEXPIRE`), and the cache lookup uses one pooled `GET`; neither
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

- The current and archived runs were taken on different host states; do not
  read the difference as a pure code regression.
- The load generator, app, PostgreSQL, Redis, and the OS share four hardware
  threads. This is a pessimistic single-box measurement.
- Unrelated processes on the workstation can consume most of the CPU. The
  clean high-rate runs were captured when the host was otherwise idle; the
  later full sweep was not and its high-rate rows are excluded.
- Results are loopback HTTP, not a real NIC or TLS termination.
- The high-rate dataset is a single hot cache entry. Cold-cache, high-cardinality,
  and missing-code shapes were measured separately; write-heavy and expiration
  workloads still have different behavior.
- `oha` is not bundled with the project; place it at `tools/oha/oha.exe` or pass
  `-OhaPath`.
- The 10k target is a design goal, not a measured production claim. Multi-core
  hosts and a separate load generator would change the ceiling.
