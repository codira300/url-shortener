# Security

## Threat model

The service is an unauthenticated URL shortener. It assumes an operator puts
authentication, authorization, and TLS in front of the administration endpoints
(`GET`/`DELETE /api/v1/urls/{code}`). The redirect endpoint is public by design.

Primary risks:

- Abuse of creation to generate spam or phishing links.
- Enumeration or scraping of codes.
- Redirect-header injection through a stored destination.
- Rate-limit bypass through forged forwarding headers.
- Dependency compromise or credential leakage.
- Resource exhaustion through connection, queue, or body floods.

## URL handling

`validateHttpUrl`:

- rejects empty strings and URLs over `kMaxUrlLength`,
- rejects ASCII control characters (`< 0x20` and `0x7f`),
- parses an absolute URI with Boost.URL,
- requires a scheme and non-empty host,
- allows only `http` and `https`.

The service never fetches the destination, so it is not an SSRF proxy. It does
store and return user-supplied URLs, so an operator should still treat them as
untrusted content and consider a reputation or blocklist check at creation time.

Before a redirect is written, the stored destination is checked for CR/LF. This
prevents header injection if a row was modified out-of-band. The response uses
`Location`, `Cache-Control: no-store`, and `Referrer-Policy: no-referrer`.

## Redirect safety

- A short code must match the base62 shape and must not be reserved.
- The response is a `302`, not a `301`, so clients do not permanently cache a
  destination that can later be deactivated.
- `Cache-Control: no-store` prevents shared caches from retaining redirects.
- `Referrer-Policy: no-referrer` avoids leaking the short URL to the destination.
- Open redirects are inherent to a shortener. Restrict creation if the service
  must only redirect to an allowlisted domain.

## Rate limiting and client identity

The limiter is keyed by scope and client identity. Identity is the peer address
unless `TRUST_PROXY` is enabled and the peer is in `TRUSTED_PROXY_CIDRS`.
Only then is `X-Forwarded-For` consulted.

Operational requirements:

- Enable `TRUST_PROXY` only behind a proxy that strips client-supplied forwarding
  headers and writes its own.
- Keep `TRUSTED_PROXY_CIDRS` as narrow as possible.
- Limit create and admin rates more aggressively than redirects.
- Treat Redis loss as a policy change: redirects fail open by default, while
  create and admin fail closed.

## Resource limits

- `HTTP_MAX_HEADER_BYTES` and `HTTP_MAX_BODY_BYTES` bound request memory.
- `HTTP_MAX_CONNECTIONS` bounds concurrent sessions.
- `WORKER_QUEUE_CAPACITY` bounds admitted work; overflow is `503`.
- Pool sizes and acquire timeouts bound dependency concurrency and wait time.
- The worker shutdown grace period bounds drain time.

TLS is not implemented in-process. Terminate it at a proxy or load balancer and
forward only trusted headers.

## Secrets

- `PG_DSN` may contain a password. It is redacted in startup logs but remains a
  secret; inject it from a secret manager, not an image layer.
- `REDIS_PASSWORD` is optional and should also come from a secret manager.
- Do not log request bodies; destinations may contain sensitive query strings.
- Access logs include method, target, status, duration, route, and peer IP.
  Enable `LOG_ACCESS` only when that data is acceptable for retention.

## Container and deployment

- Run the container as the unprivileged uid configured in the Dockerfile.
- Use a read-only root filesystem where possible.
- Do not expose PostgreSQL or Redis publicly.
- Put the administration endpoints behind authentication or a private network.
- Set `terminationGracePeriodSeconds` above `SHUTDOWN_GRACE_MS`.
- Pin dependency and base-image versions; rebuild for security updates.

## Dependency and license review

This repository has no project `LICENSE` file. Add one before distributing the
source or binaries. The main third-party components and their common licenses
are:

| Component | Typical license | Notes |
| --- | --- | --- |
| Boost | BSL-1.0 | Permissive |
| OpenSSL | Apache-2.0 (3.x) | Check the linked version |
| PostgreSQL/libpq | PostgreSQL License | Permissive |
| hiredis | BSD-3-Clause | Redis client dependency |
| redis-plus-plus | Apache-2.0 | Verify the packaged version |
| nlohmann/json | MIT | Header-only |
| Catch2 | BSL-1.0 | Test-only |
| Redis server image | Varies by tag | Redis licensing changed across 7.x/8.x; verify the exact image tag |
| PostgreSQL image | PostgreSQL License | Verify the exact image tag |

This table is a starting point, not a legal review. Generate an SBOM or use a
license scanner against the actual build inputs before release.

## Known gaps

- No authentication, authorization, tenant isolation, or audit trail.
- No CSRF concern for the JSON API because it has no cookie-authenticated
  state-changing endpoints, but adding cookie auth would require CSRF defenses.
- No malware/phishing reputation check on destinations.
- No WAF, bot detection, or per-account quota.
- No TLS termination in the application.
- No completed adversarial or fuzz test campaign for the HTTP parser or URL
  validator beyond unit tests.
