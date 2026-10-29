# Research notes

This document records the problem framing, alternatives considered, and the
measurements that changed the design. It is deliberately honest about what was
tested on this machine and what was not.

## Problem framing

A URL shortener looks simple: map a short code to a destination. The hard parts
are the operational properties around that map:

- Redirects are read-heavy and latency-sensitive; the database should not be on
  the common path.
- Creation is write-light but must be durable and collision-safe.
- A hot code can receive a large fraction of all traffic, so cache misses can
  stampede.
- Redis is a cache but also a shared dependency for rate limiting. Its failure
  must not take down all redirects.
- Overload must produce bounded queueing and explicit rejections, not memory
  growth and latency cliffs.
- A short link can be deactivated while a slow cache fill is in flight.

The target was a production-inspired service with measured throughput around
10,000 RPS on modest development hardware, not a toy CRUD endpoint.

## Technology choices

### HTTP layer

Boost.Beast over Asio was chosen because it provides HTTP/1.1 parsing and
keep-alive on top of a mature asynchronous I/O model without a framework
runtime. The server uses a fixed number of I/O threads and a separate bounded
worker pool. Alternatives considered:

- **A web framework (Drogon, Crow, Pistache):** faster to start, but hides the
  admission and lifecycle behavior this project is meant to demonstrate.
- **A raw socket server:** too much protocol surface for the value.
- **HTTP/2:** unnecessary for the benchmark and adds complexity; a TLS
  terminator in production can handle protocol upgrades.

### Storage

PostgreSQL is the source of truth because short codes need a unique constraint,
transactional writes, and durable expiration metadata. Redis is a cache and
coordination store, not a system of record. Alternatives:

- **Redis as primary storage:** fast, but durability and query semantics are
  weaker; a cache flush would lose links.
- **A relational database with no cache:** simple, but a database round trip on
  every redirect sets a much lower ceiling.
- **A dedicated KV store (DynamoDB, etc.):** viable in production, but not
  portable for a self-contained project.

### Short-code generation

Codes are random base62 strings of length 7 by default, generated in the
application, then inserted under a unique constraint. Collisions are retried up
to `SHORT_CODE_MAX_RETRIES`. The alternative - a database sequence encoded in
base62 - is collision-free but leaks creation order and couples code allocation
to the database. Random codes were preferred for opacity and simplicity; at
7 characters the space is large enough that collisions are rare, and the unique
index makes the race safe.

### Cache pattern

Cache-aside with a tagged value format:

- Positive values (`P...`) carry the destination and expiration.
- Negative values (`N`) suppress repeated lookups of missing codes.
- Tombstones (`D`) protect deletes from in-flight fills.

Two Lua scripts make populate and negative fills conditional, so a slow
database read cannot overwrite a tombstone or a positive value. These scripts
run on cache misses, not on warm-cache hits. The redirect limiter also uses a
small Lua script, described below.

### Single-flight

A per-code single-flight group coalesces concurrent cache misses. Without it, a
cold popular code produces one database query per in-flight request. The group
has a bounded wait; a timeout returns `503` rather than blocking forever.

### Rate limiting

Three approaches were implemented or evaluated:

1. **Token bucket in Lua.** Correct and smooth, but it requires a Redis round
   trip for the limiter plus another for the cache lookup. On this hardware a
   two-round-trip implementation measured around 5-6k RPS.
2. **Pipeline (limiter + cache in one round trip).** The idea was to combine
   both commands. In practice `redis-plus-plus`'s pipeline path opened a new
   Redis connection per pipeline execution. This exhausted Windows ephemeral
   ports and produced false failures. It was removed.
3. **Fixed-window counter with `INCR`.** One counter increment per request, no
   pipeline, and safe connection reuse. The window length is derived from burst
   and RPS. It allows a boundary burst of up to two windows' capacity. This is
   the production choice. The increment and first-hit expiry are now one atomic
   Lua `EVAL`, because two commands leave a crash window where a key exists
   without a TTL.

The fixed-window trade-off is documented in
[ADR 0003](../architecture/decision-records/0003-fixed-window-rate-limiter.md);
the atomicity change is recorded in
[ADR 0005](../architecture/decision-records/0005-atomic-rate-limit-increment.md).
A future version could use a sliding-window log or a token bucket with a
dedicated per-worker connection if smoother limits are required.

### Failure policy

- Redirects fail open: if Redis is unavailable, the cache lookup returns
  `Unavailable` and the service reads PostgreSQL. The limiter also fails open by
  default.
- Creates and admin operations fail closed: if the limiter cannot be consulted,
  the request returns `503`.
- A circuit breaker prevents repeated calls to a failing Redis. It opens after a
  threshold, short-circuits, then allows one probe after a cooldown.

## Measurements that changed the design

### Connection churn

A standalone reproduction with redis-plus-plus produced the decisive result:

| Workload | Redis connections after 1,000 iterations |
| --- | ---: |
| Plain `SET` + `GET` | 2 |
| `pipeline()` with `SET` + `GET` | 1,002 |
| Plain `EVAL` | 2 |

The pipeline path was the source of one connection per request. At high load it
drove `TIME_WAIT` to the edge of the 16,384-port Windows dynamic range, causing
Redis timeouts, circuit breaker flapping, and invalid benchmarks. Removing the
pipeline was more valuable than any micro-optimization.

### Load generator choice

k6 at high VU counts creates many short-lived connections and is itself
CPU-hungry on this small host. `oha` with 100 connections produced stable
measurements and exposed the real server ceiling. The k6 scenarios remain for
functional and mixed-workload tests; `oha` is used for throughput claims.

### Tuning

Pre-Lua baseline (2026-10-28):

- 64 workers / 64 Redis connections: 7.9-8.1k RPS.
- 128 workers / 128 Redis connections: 7.5k RPS (worse).
- 300 HTTP connections: 7.6k RPS (no gain; more queueing).
- Rate limiter bypassed: 9,990 RPS.

The current post-change sweep plateaued at 4.9-5.3k RPS with 64/64; 5,349 RPS
was the best single row at 10,000 offered. This is lower than the archived
baseline, partly because of the atomic Lua limiter on the hot path and partly
because the host was not in the same state. Do not compare the two as a clean
release-to-release regression.

The worker count should not simply track the connection count. Beyond a point,
context switching and queueing reduce throughput.

## What was not tested

- Multi-node deployment and network partitions.
- TLS termination and real NIC throughput.
- Redis Cluster or Sentinel failover.
- PostgreSQL replication and failover.
- Long-running soak tests and memory growth under churn.
- Adversarial URL inputs beyond the validator's unit tests.
- Docker image execution (Docker was not available in the development
  environment; the Dockerfile and compose file are provided for Linux hosts).

These are listed so the reader does not mistake the single-node measurements
for a full production qualification.
