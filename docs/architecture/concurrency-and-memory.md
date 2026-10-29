# Concurrency and memory

The service uses asynchronous I/O for sockets and a separate bounded pool for
request handlers. Every queue and pool that can grow has an explicit bound.

## Threads

| Component | Default | Role |
| --- | ---: | --- |
| HTTP I/O threads | 4 | Asio event loop, socket reads/writes |
| Worker threads | 16 | Routing, policy, cache/database calls |
| Worker queue | 4096 | Admission buffer for handler work |
| PostgreSQL pool | 16 | Bounded libpq connections |
| Redis pool | 16 | Bounded redis-plus-plus connections |
| Health thread | 1 | Readiness and process metric sampling |
| Shutdown | - | Stops admission, drains, joins |

The worker pool is fixed-size. `submit()` locks the queue, rejects when full or
when shutdown has started, and never blocks the caller. The HTTP layer maps that
rejection to `503` with `Retry-After`.

## Worker queue and shutdown

`ThreadPool::shutdown(timeout)`:

1. Sets `stopping_` and wakes all workers.
2. Waits on `drained_cv_` until the queue is empty and `active_ == 0`, or until
   the deadline.
3. Clears any queued tasks at the deadline, logs
   `thread_pool_shutdown_discarded_tasks`, and joins the workers.

The default grace period is 5 seconds and is configurable with
`SHUTDOWN_GRACE_MS`. Running tasks cannot be cancelled. Their Redis and
PostgreSQL calls are bounded by configured acquire/socket/statement timeouts, so
shutdown cannot wait forever on a healthy-but-slow dependency call.

## Single-flight

`SingleFlight<T>` is a process-local map of in-flight keys:

- The first caller becomes the leader and runs the load function synchronously.
- Joiners wait on a condition variable with a 250 ms bound.
- The leader publishes either a value or an `exception_ptr`, notifies waiters,
  and removes the entry while holding the map mutex.
- A joiner that times out returns `503`; it does not start a second load.
- The value is copied/shared through the entry, so the map does not retain the
  result after the flight completes.

The map is bounded by the number of distinct codes with concurrent misses. That
is bounded in practice by the worker count, because only workers can be inside
`resolve`. There is no cross-process coordination.

## Connection pools

- `PgPool` owns a bounded set of libpq connections. Acquire uses
  `PG_ACQUIRE_TIMEOUT_MS`; a timeout becomes `RepoStatus::Unavailable`, which
  the API maps to `503`. Broken connections are marked and replaced.
- `RedisGateway` uses redis-plus-plus connection pooling with
  `REDIS_POOL_WAIT_TIMEOUT_MS`. Errors are converted to `RedisStatus`; the
  circuit breaker prevents repeated calls to a failing endpoint.
- Pools are sized together with the worker count. A worker blocked on a pool
  acquire consumes a worker even though it is not doing useful work.

## Memory bounds

- HTTP headers and bodies have byte limits (`HTTP_MAX_HEADER_BYTES`,
  `HTTP_MAX_BODY_BYTES`).
- The worker queue has a fixed capacity; excess work is rejected, not queued.
- Cache values are bounded by the validated URL maximum plus a small header.
- Single-flight entries hold one result per in-flight code.
- Metrics are fixed-cardinality counters and histograms; route labels are
  enumerated, not user-controlled.
- Each HTTP connection consumes a session object while open; the listener caps
  concurrent connections with `HTTP_MAX_CONNECTIONS`.

There is no background expiry reaper, so PostgreSQL storage can grow with
expired rows. That is a data-retention issue, not an unbounded in-process queue.

## Shared state and atomics

- `Metrics` uses relaxed atomics for counters and gauges; rendering takes a
  consistent-enough snapshot for Prometheus.
- `HealthService` stores dependency state in atomics; the refresh thread is the
  only writer.
- `Application` increments/decrements active requests with acquire/release
  ordering while the metrics object remains alive for the process lifetime.
- The circuit breaker and pool implementations own their own synchronization;
  callers do not share mutable gateway state directly.

## Memory-safety verification

- Unit, integration, and concurrency suites run locally.
- CI has an ASan/UBSan job with PostgreSQL and Redis services.
- CI has a TSan job for the unit and concurrency suites.
- The local development host is Windows/MinGW, so local TSan is not available.
  Do not claim sanitizer coverage until the CI jobs have actually run.

## Known limits

- Single-flight is per process; N replicas can produce N cold loads for one
  code.
- The worker queue is FIFO; a slow request can delay later work in the same
  pool.
- Shutdown discards queued work after the grace period rather than returning a
  response to every accepted connection.
- No soak test has validated long-running memory stability under churn.
