# ADR 0003: Fixed-window distributed rate limiter

- Status: Accepted; implementation detail superseded by
  [ADR 0005](0005-atomic-rate-limit-increment.md)
- Date: 2026-10-28
- Supersedes: an earlier token-bucket implementation that used a Redis pipeline
  on the redirect path.

The fixed-window choice below still stands. ADR 0005 changes only how the
increment and expiry are made atomic; statements here about a separate
`PEXPIRE` or "no Lua on the hot path" describe the original implementation and
are retained as history.

## Context

The redirect path needs a distributed limit shared by replicas. The first
implementation used a Lua token bucket. To avoid two Redis round trips, a later
version combined the limiter and cache lookup in a redis-plus-plus pipeline.

Two problems emerged:

1. The pipeline path opened roughly one Redis connection per call. A standalone
   test measured 1,002 connections for 1,000 pipeline executions, versus 2 for
   plain commands.
2. The resulting connection churn exhausted Windows ephemeral ports, caused
   `redis_errors_total` spikes and circuit breaker flapping, and invalidated
   high-load benchmarks.

The fixed-window counter was chosen to remove both the extra round trip and the
pipeline.

## Decision

- Use a timestamp-derived window per scope and identity:
  `window_ms = ceil(BURST * 1000 / RPS)`, key
  `rl:v2:<scope>:<identity>:<window-id>`.
- Perform one `INCR`. If the result is 1, set `PEXPIRE` to two windows.
- Reject when the count exceeds the burst capacity; return `Retry-After` based
  on the remaining window time.
- Keep the limiter on the pooled Redis client; never pipeline it.
- Redirects fail open if Redis is unavailable; create/admin fail closed.

## Consequences

Positive:

- One cheap Redis command per request and safe connection reuse.
- No Lua on the hot path.
- The key automatically changes each window, and the TTL prevents unbounded
  growth.
- The measured error-free throughput improved from roughly 3.8k RPS (during the
  churn) to about 8.1k RPS with the limiter enabled.

Negative:

- Fixed windows allow a boundary burst of up to two windows' capacity. A client
  can send a full window at the end of one window and another at the start of
  the next.
- If a process crashes between `INCR` and `PEXPIRE`, that window's key can leak.
  The window suffix limits the impact, but the key has no TTL in that rare case.
- The limiter and cache lookup remain two Redis round trips on a warm redirect.
  That is the dominant cost versus the no-limiter ceiling.

## Alternatives

- Keep the token bucket with two round trips: smoother, but measured lower
  throughput and more Redis CPU.
- Combine the limiter and cache in one Lua script: one round trip, but more Lua
  CPU and tighter coupling. Revisit if the extra round trip becomes the primary
  bottleneck in a larger deployment.
- Client-side rate limiting: rejected; it is per-process and not enforceable
  across replicas.
- Sliding-window log: smoother than fixed windows but more memory and commands.
- Use a pipeline with a dedicated connection per worker: could work, but it
  requires careful connection ownership and was not worth the risk after the
  churn incident.
