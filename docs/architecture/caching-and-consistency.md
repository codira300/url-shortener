# Caching and consistency

Redis is a cache and a rate-limit store, never the system of record.
PostgreSQL is authoritative. The design optimizes the common redirect path while
keeping every cache state reconstructible.

## Keys and values

Cache keys are `url:v1:<short_code>`. Values use a small tagged text format:

| Value | Meaning |
| --- | --- |
| `P\|<created>\|<expires or 0>\|<url>` | Positive record |
| `N` | Negative entry for missing, expired, or inactive code |
| `D` | Delete tombstone |

The destination is the final field and may contain `|`. A malformed value is
treated as a miss and can be repaired by a database load.

## TTL policy

- Positive TTL is `min(CACHE_TTL_SECONDS, remaining lifetime)`.
- `CACHE_TTL_JITTER_PERCENT` applies jitter so a large set of entries created
  together does not expire together.
- A positive TTL is clamped to at least 1 ms and at most the Redis maximum.
- Negative TTL is `CACHE_NEGATIVE_TTL_SECONDS` and is intentionally short.
- Tombstone TTL is `CACHE_TOMBSTONE_TTL_SECONDS`.

## Read path

`UrlService::resolve`:

1. `GET` the cache key.
2. Positive and unexpired: return immediately.
3. Negative or tombstone: return `404` without PostgreSQL.
4. Miss, malformed value, or Redis unavailable: enter single-flight and load
   PostgreSQL.

On a database result:

- Active and unexpired: `populate()` then return.
- Missing, inactive, or expired: `storeNegative()` then return `404`.
- Database unavailable: return `503`; nothing is cached.

Single-flight is process-local and keyed by code. It has a 250 ms wait bound.
The leader loads the database; joiners wait for the result. A timeout returns
`503` rather than blocking a worker indefinitely. It does not coordinate
replicas; the database pool and negative caching limit cross-process load.

## Tombstone-safe fills

Two Lua scripts make cache writes conditional:

```lua
-- populate
local v = redis.call('GET', KEYS[1])
if v and string.sub(v, 1, 1) == 'D' then
  return 0
end
redis.call('SET', KEYS[1], ARGV[1], 'PX', ARGV[2])
return 1
```

```lua
-- negative
local v = redis.call('GET', KEYS[1])
if v then
  local tag = string.sub(v, 1, 1)
  if tag == 'P' or tag == 'D' then
    return 0
  end
end
redis.call('SET', KEYS[1], 'N', 'PX', ARGV[1])
return 1
```

The populate script refuses to overwrite a tombstone. This closes the ordinary
race where a database read starts before a delete and finishes after the
tombstone is written. `storeCreated` uses the same conditional script for
defense in depth; a refused create fill is logged and counted.

## Delete consistency

Delete performs two separate systems:

1. PostgreSQL: set `is_active = false` and `deactivated_at = now()`.
2. Redis: write `D` with a bounded TTL.

The database is authoritative immediately. The cache write is retried three
times. If all attempts fail, `DELETE` returns `503` even though the row is
inactive. A retry converges. If the process dies after the database commit and
before the tombstone, a positive cache entry can survive until its bounded TTL.
This is **eventual revocation**, not strict revocation. The API documentation
and tests describe that trade-off rather than claiming linearizable delete.

## Redis failure behavior

| Redis state | Cache read | Redirect | Create/meta/delete |
| --- | --- | --- | --- |
| Healthy | Normal | Cache hit or database fallback | Limiter enforced |
| Unavailable | `Unavailable` | Fail open to PostgreSQL | `503` (limiter fail-closed) |
| Slow / failing | Circuit opens | Fail open quickly | `503` until probe succeeds |

The circuit breaker prevents every worker from waiting on a sick Redis. Cache
unavailability and invalidation failures increment separate counters, and
tombstone refusals have their own counter.

## What is not guaranteed

- Cross-replica single-flight. Two replicas can each load the same cold code.
- Strict delete visibility. A stale positive entry can survive its bounded TTL.
- Cache coherence with direct database edits. All writes should go through the
  service; an out-of-band SQL update is not observed until the cache expires.
- Durability from Redis. A flush loses warmth and limiter state, not links.

## Metrics to watch

- `urlshort_cache_hits_total`, `misses_total`, `negative_hits_total`
- `urlshort_cache_unavailable_total`
- `urlshort_cache_tombstone_prevented_total`
- `urlshort_singleflight_joined_total`
- `urlshort_redis_errors_total`, `redis_circuit_open_total`
- `urlshort_redis_command_duration_seconds_bucket`
