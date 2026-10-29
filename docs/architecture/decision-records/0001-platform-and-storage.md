# ADR 0001: C++20, Boost.Beast, PostgreSQL, and Redis

- Status: Accepted
- Date: 2026-10-28

## Context

The service must demonstrate production concerns, not just a URL mapping. It
needs a stable HTTP/1.1 server, durable writes, a fast read path, and enough
control to reason about overload. The benchmark hardware is small (four logical
cores), so unnecessary framework layers are costly.

## Decision

- C++20 with CMake.
- Boost.Beast over Asio for HTTP/1.1 and asynchronous socket I/O.
- A fixed number of I/O threads plus a separate bounded worker pool.
- PostgreSQL via libpq for durable storage and the unique short-code constraint.
- Redis via redis-plus-plus for cache-aside reads and distributed rate limiting.
- Prometheus text metrics and structured JSON logs.

## Consequences

Positive:

- Direct control over connection lifetime, admission, timeouts, and pools.
- PostgreSQL gives transactional correctness for creates and deactivations.
- Redis removes the database from warm redirects.
- The binary is self-contained and portable across Linux and Windows.

Negative:

- More code than a web framework, especially around HTTP parsing and lifecycle.
- The team owns pool sizing, backpressure, and failure semantics.
- redis-plus-plus behavior must be verified at the connection level; a
  high-level abstraction hid the pipeline churn described in ADR 0003.

## Alternatives

- A batteries-included framework (Drogon/Crow): rejected because it obscures the
  operational behavior this project is meant to show.
- Redis as primary storage: rejected for durability and query reasons.
- Database-only reads: rejected because the measured ceiling would be dominated
  by database round trips.
- A managed shortener API: rejected because the exercise is the implementation.
