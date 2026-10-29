# System overview

This is the top-level map. Detailed behavior is split across
[request lifecycle](request-lifecycle.md), [data model](data-model.md),
[caching and consistency](caching-and-consistency.md),
[concurrency and memory](concurrency-and-memory.md),
[scalability and capacity](scalability-and-capacity.md), and
[security](security.md).

## Goals

- Serve redirects from memory-adjacent Redis on the common path.
- Keep PostgreSQL as the authoritative, durable record.
- Degrade predictably when Redis or PostgreSQL is unhealthy.
- Bound resource use under overload instead of queueing without limit.
- Remain stateless at the HTTP tier so replicas can be added horizontally.

## Component view

```
                         +-----------------------------+
  clients --- HTTP/TCP ->| Boost.Beast / Asio I/O      |
                         |  keep-alive, timeouts       |
                         +--------------+--------------+
                                        |
                                        | bounded admission
                                        v
                         +-----------------------------+
                         | Worker thread pool          |
                         |  routing + policy           |
                         +----+-------------+----------+
                              |             |
                    rate limit|             |resolve / create
                              v             v
                    +----------------+  +---------------------+
                    | Redis          |  | UrlService          |
                    |  EVALSHA       |  |  cache -> PG        |
                    |  GET cache     |  |  single-flight      |
                    +-------+--------+  +----------+----------+
                            |                      |
                            |                      v
                            |            +---------------------+
                            |            | PostgreSQL pool     |
                            |            |  urls table         |
                            |            +---------------------+
                            |
                    +-------v--------+
                    | Circuit breaker|
                    |  fail-open     |
                    +----------------+
```

The process owns one Redis client with a connection pool. PostgreSQL uses a
separate pool. The HTTP I/O threads only parse and dispatch; handler work runs
on the worker pool.

## Request lifecycle

The concise flow is below; [request lifecycle](request-lifecycle.md) covers
admission, every endpoint, and failure mapping.

1. Boost.Beast parses the request. Malformed or oversized requests are rejected
   before handler dispatch.
2. The request is submitted to the bounded worker queue. If the queue is full,
   the server returns `503` with `Retry-After: 1` and increments a rejection
   metric.
3. `Application::routeAndHandle` matches the path and method.
4. For redirects:
   - Validate the short-code shape and reserved-name list.
   - Compute the client identity (peer address or trusted `X-Forwarded-For`).
   - Enforce the distributed redirect rate limit.
   - Resolve through Redis, falling back to PostgreSQL on a miss through a
     per-code single-flight group.
   - Return `302` with no-store headers and the stored destination.
5. The response is written and the active-request gauge is decremented. Route,
   status, and latency are recorded in the Prometheus histogram.

## Data model

`migrations/001_init.sql` defines a single `urls` table:

| Column | Type | Notes |
| --- | --- | --- |
| `id` | `BIGSERIAL` | Internal primary key |
| `short_code` | `TEXT` | Unique, 1-16 base62 characters |
| `original_url` | `TEXT` | Non-empty destination |
| `is_active` | `BOOLEAN` | False after deactivation |
| `created_at` | `TIMESTAMPTZ` | Defaults to now |
| `expires_at` | `TIMESTAMPTZ` | Nullable |
| `deactivated_at` | `TIMESTAMPTZ` | Set when deactivated |

Indexes cover active expirations and creation time. The unique constraint on
`short_code` is the collision detector; code generation retries on conflict.

## Cache protocol

Keys use the prefix `url:v1:<short_code>`. Values are compact tagged strings:

- `P...` positive record (destination + expiration).
- `N` negative cache entry for a missing or expired code.
- `D` tombstone written on delete.

Positive TTL is the smaller of `CACHE_TTL_SECONDS` and the remaining URL
lifetime, with configurable jitter. Negative TTL is short. Delete writes a
tombstone with `CACHE_TOMBSTONE_TTL_SECONDS`.

Two small Lua scripts make fills safe:

- **Populate:** do not overwrite `D`; otherwise `SET` with TTL.
- **Negative:** do not overwrite `P` or `D`; otherwise `SET N` with TTL.

This prevents a database read that started before a delete from resurrecting
the URL after the tombstone was written.

The cache is not required for correctness. A miss or Redis failure falls back
to PostgreSQL. The fallback is protected by single-flight so a cold or
thundering-herd code produces one database query per process, not one per
request. Delete revocation is **eventual**, not strict: the database is
authoritative immediately, while a positive Redis entry can survive until its
bounded TTL if invalidation cannot be written. See
[caching and consistency](caching-and-consistency.md).

## Rate limiting

The redirect, create, and metadata/delete paths use separate scopes. The
limiter is a fixed-window counter in Redis:

```
window_ms = ceil(BURST * 1000 / RPS)
key       = rl:v2:<scope>:<identity>:<window-id>
```

A request performs one atomic Lua script via `EVALSHA`: it executes `INCR`, and
only when the returned count is `1` it executes `PEXPIRE` for two windows.
Combining the commands avoids a crash window in which a counter exists without a
TTL. The script is loaded once and reloaded on `NOSCRIPT`. Counts
above the burst capacity return `429` with a `Retry-After` derived from the
remaining window time.

Redirects are fail-open by default: if Redis cannot answer, the request proceeds
to the cache/database path. Create and admin paths are fail-closed and return
`503` when the limiter is unavailable.

Why not a token bucket? A token bucket needs either another Redis round trip or
a combined script, and it produces smoother traffic at the cost of more state
per client. The fixed window keeps one counter and one round trip, and it avoids
historical pipeline connection churn in the Redis client. The trade-off is a
boundary burst of up to two windows' capacity. See
[ADR 0003](decision-records/0003-fixed-window-rate-limiter.md) and
[ADR 0005](decision-records/0005-atomic-rate-limit-increment.md).

## Failure handling

| Failure | Redirect behavior | Create/meta/delete behavior |
| --- | --- | --- |
| Redis unavailable | Fail open, read PostgreSQL | Limiter unavailable -> `503` |
| Redis latency spike | Circuit opens after threshold; fail open | `503` until cooldown probe |
| PostgreSQL unavailable | `503` with `Retry-After` | `503` with `Retry-After` |
| Worker queue full | `503` with `Retry-After` | `503` with `Retry-After` |
| Duplicate short code | Retry generation, up to limit | `503` if exhausted |
| Cache invalidation fails on delete | Not applicable | `503`; row is inactive, retry converges |

The Redis circuit breaker has three states: closed, open, and half-open. In the
open state calls fail immediately; after the cooldown a single probe is allowed.
A successful probe closes the circuit. Metrics expose errors and circuit opens.

## Concurrency model

- **I/O threads** (`HTTP_IO_THREADS`) run the Asio event loop and socket I/O.
- **Worker threads** (`WORKER_THREADS`) execute request handlers. The pool has a
  bounded queue (`WORKER_QUEUE_CAPACITY`); overflow is rejected.
- **PostgreSQL pool** (`PG_POOL_SIZE`) bounds database connections. Acquire
  timeouts turn pool exhaustion into a fast `503`, not unbounded waiting.
- **Redis pool** (`REDIS_POOL_SIZE`) bounds Redis connections. `redis-plus-plus`
  reuses pooled connections; the hot path never creates a pipeline.
- **Single-flight** maps per short code so concurrent cache misses for one code
  share one database load.

The pools must be sized together with the worker count. A worker blocked on a
pool acquire consumes a worker; the acquire timeout is therefore shorter than
the request timeout. Shutdown stops admission, drains queued work for
`SHUTDOWN_GRACE_MS`, discards work that remains queued at the deadline, and
joins the workers. In-flight dependency calls are bounded by their own timeouts.
See [concurrency and memory](concurrency-and-memory.md).

## Security boundaries

- URL validation accepts only absolute `http`/`https` URLs with a host, rejects
  control characters, and enforces a maximum length. Response headers are
  checked for CR/LF before use.
- `TRUST_PROXY` is off by default. When enabled, only addresses in
  `TRUSTED_PROXY_CIDRS` may supply `X-Forwarded-For`.
- There is no authentication. Deploy the metadata and delete endpoints behind
  authentication or a private network.
- The service does not fetch destination URLs, so it is not an SSRF proxy.
- See [security](security.md) for the threat model and deployment checklist.

## Scaling notes

The HTTP tier is stateless. Scale it horizontally and point all replicas at the
same Redis and PostgreSQL. Redis is the shared hot path and rate-limit store;
use a managed/replicated deployment with persistence appropriate to the cache
and limiter semantics. PostgreSQL remains the only durable source of truth. See
[scalability and capacity](scalability-and-capacity.md) for measured limits.
