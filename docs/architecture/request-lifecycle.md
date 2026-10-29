# Request lifecycle

This document follows a request from the listening socket to a response and
records the status mapping that the API promises.

## Admission

1. Boost.Beast parses the request with configured header and body limits. A
   malformed or oversized request is rejected by the HTTP layer before routing.
2. `HttpServer` submits the handler to the bounded worker queue. If the queue is
   full or the pool is stopping, the request receives `503` with
   `Retry-After: 1` and the queue-rejection counter is incremented. Submission
   never blocks an I/O thread.
3. `Application::handle` increments the active-request gauge, starts the latency
   timer, and calls `routeAndHandle`.
4. After the response is produced, `handle` records the route/status histogram,
   optional access log, and decrements the active-request gauge. Unexpected
   exceptions become `500` without leaking internal details.

## Routes

| Method | Path | Route label | Purpose |
| --- | --- | --- | --- |
| `GET` | `/{code}` | `redirect` | Resolve and redirect |
| `POST` | `/api/v1/urls` | `create` | Create a short URL |
| `GET` | `/api/v1/urls/{code}` | `metadata` | Read metadata |
| `DELETE` | `/api/v1/urls/{code}` | `delete` | Deactivate a short URL |
| `GET` | `/health/live` | `health` | Liveness |
| `GET` | `/health/ready` | `health` | Dependency readiness |
| `GET` | `/metrics` | `metrics` | Prometheus exposition |

The router normalizes origin-form and absolute-form targets, drops query strings
and fragments, and rejects extra path segments. Reserved names such as `api`,
`health`, and `metrics` cannot be redirected. A wrong method on a known path
returns `405` with `Allow`.

## Redirect

1. Validate the code shape and reserved-name list. Invalid codes return `404`.
2. Resolve the client identity from the peer address, or from
   `X-Forwarded-For` only when the peer is in `TRUSTED_PROXY_CIDRS`.
3. Check the `redirect` fixed window. A limit breach returns `429` with
   `Retry-After`; a Redis failure is fail-open by default.
4. `UrlService::resolve` performs one cache `GET`.
   - Positive hit: return the cached record.
   - Negative hit or tombstone: return `404` without PostgreSQL.
   - Miss or Redis failure: enter the per-code single-flight group and load
     PostgreSQL.
5. A successful database load populates Redis and returns the record. A missing,
   expired, or inactive row writes a negative cache entry and returns `404`.
6. Before writing `Location`, reject a stored destination containing CR/LF.
7. Return `302` with `Location`, `Cache-Control: no-store`, and
   `Referrer-Policy: no-referrer`.

A cache hit on this path performs one Redis `GET` and one rate-limit `EVAL`; it
does not touch PostgreSQL.

## Create

1. Check the `create` fixed window. This limiter is fail-closed.
2. Parse JSON, require an object with a string `url`, and require `expires_at`
   to be `null` or an RFC 3339 timestamp with a timezone.
3. Validate that the destination is an absolute `http`/`https` URL with a host,
   no control characters, and a bounded length. `expires_at` must be in the
   future.
4. Generate a random base62 code and insert it. A unique-constraint collision is
   retried up to `SHORT_CODE_MAX_RETRIES`; exhaustion returns `503`.
5. On success, best-effort populate Redis through the tombstone-safe Lua script.
   A cache failure does not fail the create because PostgreSQL is durable.
6. Return `201` with the record representation and a `Location` header.

Validation failures return `422`; malformed JSON returns `400`.

## Metadata

1. Check the `admin` fixed window (fail-closed).
2. Read PostgreSQL directly. Metadata is not served from the cache.
3. Return `200` for an active, unexpired row; `410` for an inactive or expired
   row; `404` when no row exists; `503` when PostgreSQL is unavailable.

## Delete

1. Check the `admin` fixed window (fail-closed).
2. Deactivate the row. The row is retained for audit and expiration history.
   Deleting an already inactive row is idempotent and returns success.
3. Write a `D` tombstone with the configured tombstone TTL. The write is retried
   three times with short backoff.
4. If the tombstone cannot be written, return `503` even though the database row
   is already inactive. The client can retry; the retry writes the tombstone and
   converges. This is eventual revocation, not strict revocation: a positive
   Redis entry can survive until its bounded TTL if the process dies between the
   database update and the tombstone write.
5. Return `204` on success.

## Health and metrics

- `/health/live` is process-local and always `200` while the server is running.
- `/health/ready` samples PostgreSQL and Redis. It returns `200` only when both
  are up; otherwise `503`. `degraded` is true when PostgreSQL is up and Redis is
  down. Readiness is sampled at `READINESS_REFRESH_MS`, not on every request.
- `/metrics` renders counters, latency histograms, dependency histograms,
  process CPU/RSS/threads, and the readiness gauge.

## Error envelope

All JSON errors use:

```json
{"error":{"code":"rate_limited","message":"too many requests"}}
```

`503` responses carry `Retry-After: 1`. `500` responses deliberately omit
internal exception text; details go to the structured log.

## Shutdown

On `SIGTERM`/Ctrl+C, the HTTP server stops accepting new work. The worker pool
waits up to `SHUTDOWN_GRACE_MS` for queued and running tasks, then discards
queued tasks, logs their count, and joins the workers. Running tasks cannot be
cancelled; their dependency calls are bounded by Redis and PostgreSQL timeouts.
