# Repository review

Reviewed on 2026-10-29 to compare this implementation with public C++ URL
shorteners. Candidate discovery used GitHub repository search for
`url shortener language:C++` and then metadata, README, and source-tree
inspection. Star counts and push dates are observations from that date; they are
not quality claims.

The goal was not to copy code. It was to check whether the same problems
(cache stampede, durability, deletion, limits, licensing) are handled elsewhere
and what this project should make explicit.

## Summary

| Repository | Shape | Storage | Notable choices | License |
| --- | --- | --- | --- | --- |
| `Prashant0664/Advanced-C-URL-Shortener-Microservice` | Modern C++ HTTP microservice | MySQL X DevAPI | httplib, OAuth, sessions, token-bucket limits, analytics | None declared |
| `yuvii-b/url-shortener` | Crow web UI + CLI | SQLite | Hash-based codes, built-in HTTP server | MIT |
| `heapwolf/redirector` | libuv/http_parser server | LevelDB | Compact C++ server, returns existing code for a duplicate URL | None declared |
| `abrutus/url-shortener` | Minimal command-line program | Kyoto Cabinet | Very small, disk-backed key/value store | None declared |
| `FONQRI/URLShortener` | Qt/QtWebApp application | Data files via Qt | Desktop-era server stack, LGPL dependency | LGPL-3.0 |
| `adityasonwane555/URL-Shortener` | Console application | In-memory maps | Base62 over an incrementing ID, reverse map for deduplication | None declared |

Also encountered but not used as primary comparisons: `motoishmz/ofxShortURL`
(a bit.ly API wrapper, not a service), `hq6/aws-lambda-url-expander` (a URL
expander), and several small course projects with no documented build or
license.

## Repository notes

### Prashant0664/Advanced-C-URL-Shortener-Microservice

- HttpLib + MySQL X DevAPI, CMake/vcpkg, Dockerfile.
- Implements Google OAuth, bearer tokens, sessions, favourites, click counts,
  admin routes, and a token-bucket limiter.
- The README describes a broad feature set; the inspected tree is a single
  server/DB pair rather than a layered service.
- No license file was found. Do not reuse code without permission.

Takeaway: authentication and analytics are product features, not substitutes
for storage consistency or operational limits. This project intentionally leaves
auth to the edge and concentrates on the URL lifecycle, cache safety, and
failure behavior.

### yuvii-b/url-shortener

- Crow HTTP server, SQLite persistence, terminal UI, small web frontend.
- Codes are derived from a hash of the URL rather than a random allocation.
- MIT licensed, so the design is safe to study and reuse with attribution.

Takeaway: a hash-derived code is deterministic and can deduplicate URLs, but it
needs collision handling and can leak correlation. Random codes under a unique
constraint avoid that coupling; this project chooses random base62 codes and
retries collisions.

### heapwolf/redirector

- libuv + http_parser + LevelDB; compact server and a `shorten` endpoint.
- A duplicate URL returns the existing code.
- Redirects use `301 Moved Permanently`.
- No license file was found.

Takeaway: `301` is efficient but wrong for links that can be deleted or expired;
clients and caches may retain it. This project uses `302` plus `no-store` because
revocation is part of the lifecycle.

### abrutus/url-shortener

- Very small C++ program using Kyoto Cabinet as the database.
- Build instructions rely on a system-installed legacy library and a generated
  `casket.kch` file.
- No license file was found.

Takeaway: an embedded key/value store removes operational dependencies but
gives up the unique-constraint, SQL inspection, and backup ecosystem used here.
It is a good minimal reference, not a production architecture.

### FONQRI/URLShortener

- QtWebApp-based server with templates and a config file.
- LGPL-3.0, with an explicit notice about the modified QtWebApp portion.
- No separate cache, distributed limiter, or health model.

Takeaway: a framework can shorten time-to-first-request, but it hides the
admission, timeout, and shutdown behavior this project is meant to demonstrate.
The LGPL notice is also a reminder to preserve upstream license obligations.

### adityasonwane555/URL-Shortener

- Single-file console application, in-memory `unordered_map` in both directions.
- Converts an incrementing integer to base62 and deduplicates by URL.
- No persistence, HTTP server, or license file.

Takeaway: base62 over a sequence is compact and collision-free, but it leaks
creation order and couples allocation to a counter. Random codes plus a unique
index were chosen for opacity and multi-process safety.

## Cross-repository findings

1. **Most examples are storage demos, not services.** Several have no HTTP
   server, no failure policy, and no tests. The hard part is not the mapping; it
   is cache coherence, overload, and dependency failure.
2. **A single embedded store is common.** SQLite, LevelDB, Kyoto Cabinet, and
   in-memory maps all appear. They simplify deployment but often lose the
   transactional uniqueness and operational visibility that PostgreSQL provides.
3. **Redirect status is often treated casually.** A permanent redirect is easy
   to emit but conflicts with deletion and expiry. This project's `302` +
   `no-store` choice is deliberate.
4. **License metadata is frequently missing.** Four of the six primary
   repositories declare no license. Absence of a license is not permission to
   copy. GUI/Qt projects also carry transitive LGPL obligations.
5. **Authentication is usually all-or-nothing.** One repository adds OAuth and
   session management, while others expose everything. This project documents
   the lack of auth explicitly and requires edge protection for admin routes.
6. **Testing and observability are rare.** The reviewed projects mostly lack
   queue bounds, metrics, readiness, or failure-injection tests. Those are the
   differentiators this repository invests in.

## What this project adopted

- Durable relational storage with a unique constraint for code allocation.
- A cache-aside read path with negative entries and tombstones.
- `302` rather than `301` for revocable links.
- A bounded worker queue and explicit `503` under overload.
- Dependency failure policies split by operation: redirects fail open, writes
  fail closed.
- A single fixed-window counter per client and window, with an atomic Lua
  increment.
- Explicit license and third-party review before distribution.

## What remains unproven

- No reviewed repository provided a reusable production benchmark, so the
  performance claims here come from this project's own harness.
- No multi-node, Redis Cluster, or PostgreSQL failover comparison was completed.
- The review is a sample, not a systematic survey of all 248 search results.
- License findings are based on repository metadata and root files, not legal
  advice; re-check the exact commit before reuse.
