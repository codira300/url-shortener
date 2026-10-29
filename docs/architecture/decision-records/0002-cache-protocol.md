# ADR 0002: Cache-aside with tagged values, tombstones, and single-flight

- Status: Accepted
- Date: 2026-10-28

## Context

The redirect path must avoid PostgreSQL on the common path. A naive cache-aside
implementation has two correctness hazards:

1. A slow database read can repopulate a URL after a concurrent delete.
2. A missing or expired code can cause repeated database lookups under load.

There is also a thundering-herd risk when a popular code's cache entry expires.

## Decision

- Cache key: `url:v1:<short_code>`.
- Tagged values: `P` positive, `N` negative, `D` tombstone.
- Positive TTL is the smaller of the cache TTL and the URL's remaining lifetime,
  with jitter.
- Delete writes a tombstone with a short TTL instead of deleting the key.
- Conditional fills use two small Lua scripts:
  - populate refuses to overwrite `D`;
  - negative fill refuses to overwrite `P` or `D`.
- Cache misses are coalesced per code by a single-flight group with a bounded
  wait.

## Consequences

Positive:

- Warm redirects never touch PostgreSQL.
- Deletes cannot be undone by an in-flight cache fill.
- Repeated misses are cheap and single-flight prevents a cold code from
  stampeding the database.
- The tombstone eventually expires, so the cache does not retain deleted data
  forever.

Negative:

- A deleted code stays tombstoned for `CACHE_TOMBSTONE_TTL_SECONDS`; after the
  tombstone expires, the first read goes to PostgreSQL and confirms the delete.
- Negative entries can briefly hide a URL created immediately after a miss;
  the short negative TTL bounds the window.
- Lua is used on write/fill paths. It is not used on the redirect read path.
- A Redis outage means every redirect hits PostgreSQL; single-flight helps but
  does not remove the load.

## Alternatives

- Delete the key on delete: rejected because a concurrent fill can resurrect it.
- Write-through every create: considered, but cache-aside plus a post-create
  store keeps the write path explicit and handles cache failure gracefully.
- No negative caching: rejected because nonexistent-code scans would amplify
  into the database.
- Distributed locking for misses: rejected as heavier than an in-process
  single-flight group; cross-replica duplicate loads are acceptable and bounded.
