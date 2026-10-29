# ADR 0004: Bounded admission and explicit failure policy

- Status: Accepted
- Date: 2026-10-28

## Context

Under overload, an unbounded queue turns a latency problem into a memory and
availability problem. When Redis or PostgreSQL is unhealthy, the service must
choose between failing every request and serving what it safely can.

## Decision

- The HTTP layer submits handler work to a worker pool with a fixed thread count
  and a bounded queue (`WORKER_THREADS`, `WORKER_QUEUE_CAPACITY`).
- When the queue is full, reject with `503` and `Retry-After` rather than
  blocking the I/O thread or growing the queue.
- PostgreSQL and Redis pools have acquire/wait timeouts shorter than the HTTP
  timeouts, so pool exhaustion fails fast.
- Failure policy:
  - Redirects fail open: missing Redis means read PostgreSQL; limiter failure
    means allow (configurable).
  - Creates, metadata, and deletes fail closed: limiter failure is `503`.
- A Redis circuit breaker opens after repeated failures and probes after a
  cooldown.
- Readiness is separate from liveness. Redis-only failure is reported as
  degraded but does not restart the process.

## Consequences

Positive:

- Memory and latency remain bounded under overload.
- A Redis outage does not take down redirects.
- Write operations never bypass their limiter and cannot amplify an outage.
- Health checks distinguish "process alive" from "dependencies ready."

Negative:

- Clients see `503` during overload; they must honor `Retry-After`.
- Fail-open redirects can shift load to PostgreSQL during a Redis outage.
- The circuit breaker adds state and a cooldown tuning parameter.
- The queue capacity and thread count require measurement; the defaults are not
  universal.

## Alternatives

- Unbounded queue: rejected; it hides overload until memory or latency collapses.
- Fail-closed redirects: rejected as the default because a cache outage would
  turn a degradation into a full outage. It remains configurable.
- Synchronous handling on I/O threads: rejected; slow dependencies would block
  all sockets.
- One shared pool for all dependencies: rejected; PostgreSQL and Redis have
  different latency and failure characteristics.
