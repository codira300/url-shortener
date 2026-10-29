# Scalability and capacity

## Measured envelope

The current post-change runs used a 2-core / 4-thread Intel i5-7300U with
8 GB RAM, a co-located `oha` generator, local PostgreSQL, and local Redis. With
the atomic Lua limiter using cached `EVALSHA`, three consecutive 10,000-offered
runs reached **7,195, 8,062, and 7,480 RPS** (median **7,480 RPS**). The 10,000
RPS goal was not reached in the production configuration on that host.

An archived pre-Lua baseline on the same class of hardware reached about 8.1k
RPS with the limiter enabled and 9,990 RPS with it bypassed. That baseline was
captured under different host conditions and is historical context, not a
current release comparison. Full setup and caveats are in
[performance results](../performance/results.md).

## Where the time goes

A warm redirect does:

1. One Redis `EVALSHA` for the fixed-window limiter.
2. One Redis `GET` for the cache entry.
3. One HTTP response write.

A cold redirect adds a PostgreSQL lookup and a cache fill, protected by
single-flight per process. The database is not on the warm path, but it becomes
the bottleneck if Redis fails because every redirect falls back to it.

On the same host, a 200-URL cold start at 10 connections sustained 1,777 RPS
with p95 21 ms and 200/200 `302` responses. A 500-key high-cardinality run at
2,500 offered RPS reached 2,496 RPS with p95 6 ms. These are workload-shape
checks under host contention, not clean capacity numbers; see
[performance results](../performance/results.md).

The observed ceiling is therefore a combination of:

- Redis round-trip latency and connection-pool wait.
- Worker context switching and queueing.
- HTTP parsing/writing on four shared hardware threads.
- Load-generator CPU competing with the service.

In the pre-change baseline, more workers did not help: 128 workers / 128 Redis
connections measured about 7.5k RPS versus 7.9-8.1k with 64/64, and more HTTP
connections also did not help. The current high-rate runs used 64/64 and
plateaued at about 7.5k-8.1k RPS. This is a queueing/context-switch result, not
a reason to keep increasing threads.

## Scaling model

The HTTP tier is stateless. To scale reads:

1. Run more replicas behind a load balancer.
2. Point all replicas at the same Redis and PostgreSQL.
3. Size each replica's worker and connection pools to its CPU and dependency
   latency.
4. Keep each replica's single-flight local; accept up to N cold database reads
   for a hot code during a cache miss.

Redis becomes the shared throughput limit first. A single Redis instance can
serve a large number of simple commands, but the limiter and cache both use it
on every redirect. Use a managed/replicated deployment, monitor command latency,
and avoid scripts with unbounded key sets. The limiter script touches one key,
so a cluster can route by key without multi-slot operations.

PostgreSQL scales writes and cold reads. Creates are write-light but each one
needs a unique insert and possibly collision retries. Metadata and delete are
database operations. If Redis is down, redirects fail open to PostgreSQL and can
overwhelm it; shed load at the edge instead of assuming the database can absorb
the entire redirect rate.

## Sizing starting points

| Resource | Starting point | Notes |
| --- | --- | --- |
| Workers | 2-4x CPU cores | I/O-heavy; measure rather than guess |
| Worker queue | 1024-4096 | Smaller queues fail faster |
| PostgreSQL pool | Close to concurrent DB callers | 16 on a small host |
| Redis pool | Close to concurrent Redis callers | 16-64 measured range |
| Redis acquire timeout | Below HTTP timeout | Fail fast under saturation |
| PostgreSQL acquire timeout | Below HTTP timeout | 250 ms default |

The benchmark used 64 workers and 64 Redis connections because the host had four
logical CPUs and a co-located generator. That is not a universal recommendation.

## What does not scale

- A single Redis instance with no failover.
- A single PostgreSQL primary with no read strategy for cold-cache storms.
- One process handling both load generation and serving.
- Pipeline-per-request Redis usage, which caused connection churn on Windows.
- Unbounded queues, which convert overload into latency and memory growth.

## Capacity questions to answer before production

- What is the p99 Redis command latency under the expected command mix?
- What happens to PostgreSQL load during a Redis outage?
- How many cold misses per second can the database pool absorb?
- What is the eviction policy for cache entries and limiter counters?
- What is the retention policy for expired rows?
- How many replicas are needed to meet the SLO with one dependency degraded?

No multi-node, TLS, or network-partition benchmark has been completed. Do not
extrapolate the single-host numbers into a production claim.
