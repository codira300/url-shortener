# ADR 0005: Atomic rate-limit increment

- Status: Accepted
- Date: 2026-10-29
- Supersedes in part: [ADR 0003](0003-fixed-window-rate-limiter.md), which kept
  the fixed-window design but performed `INCR` and `PEXPIRE` as separate calls.

## Context

ADR 0003 replaced a Redis pipeline with a fixed-window counter: `INCR` the
current window key, then `PEXPIRE` it on the first hit. That is one logical
operation but two Redis commands. If the process dies or the connection is lost
between them, the key can remain without a TTL. The window suffix bounds the
damage, but it violates the intended "every limiter key expires" invariant and
complicates incident reasoning.

The limiter also needs to return the post-increment count in the same operation
so it can decide whether to reject without a second read.

## Decision

Use one Lua script per check. The gateway loads it once with `SCRIPT LOAD`,
executes it with `EVALSHA`, and reloads on `NOSCRIPT` (for example after a Redis
restart or `SCRIPT FLUSH`):

```lua
local count = redis.call('INCR', KEYS[1])
if count == 1 then
  redis.call('PEXPIRE', KEYS[1], ARGV[1])
end
return count
```

- The script receives one key and one argument (two windows in milliseconds).
- `INCR` and the conditional first-hit `PEXPIRE` are atomic on the Redis server.
- The returned integer is the same value used for the limit decision.
- The key remains `rl:v2:<scope>:<identity>:<window-id>`.
- Fail-open/fail-closed policy is unchanged.

## Consequences

Positive:

- No crash window where a counter exists without a TTL.
- One Redis round trip per limiter check, with a single command in the client.
- The script touches one key, so it remains compatible with key-based cluster
  routing.
- The result is authoritative for the reject/allow decision.

Negative:

- Every limiter check still invokes Lua, which has some server-side overhead
  compared with a bare `INCR`. Cached `EVALSHA` avoids resending and re-hashing
  the script, but not Lua execution.
- The script must be reviewed as code; an error changes the limiter semantics.
- A Redis restart or `SCRIPT FLUSH` invalidates cached SHAs. The gateway handles
  the resulting `NOSCRIPT` error by reloading the script once.
- More than one app instance may race to load the script; Redis returns the same
  SHA, so the operation remains idempotent.

## Alternatives

- Keep `INCR` and `PEXPIRE` separate: simplest, but leaves the TTL invariant
  vulnerable to a crash or disconnect.
- Use `MULTI`/`EXEC`: commands are atomic as a group, but the conditional
  "only expire on count 1" still needs logic; a transaction does not remove the
  need to branch.
- Set the TTL unconditionally on every request: avoids the branch but adds a
  write and extends the window on every request, which changes the window
  semantics.
- Move the limiter to a token bucket: smoother, but more state and either another
  round trip or a larger script. Not justified by the evidence gathered here.

## Verification

- `tests/integration/test_integration.cpp` checks the Lua limiter behavior and
  key TTL.
- `tests/concurrency/test_concurrency.cpp` exercises the limiter with real
  backends.
- `tests/integration/test_integration.cpp` flushes Redis scripts and confirms
  that the next `evalInt` reloads the SHA and succeeds.
- The current Release sweep ran after this change. Three 10,000-offered runs
  reached 7,195, 8,062, and 7,480 RPS with zero errors; details are in
  [performance results](../../performance/results.md).
- A standalone Redis microbenchmark measured 17.2-17.7k commands/s for `EVAL`
  and 21.2-21.5k for `EVALSHA`.
