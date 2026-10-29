# Data model

PostgreSQL is the source of truth. Redis contains only reconstructible cache
entries and rate-limit counters.

## `urls` table

Defined by [`migrations/001_init.sql`](../../migrations/001_init.sql).

| Column | Type | Constraints | Meaning |
| --- | --- | --- | --- |
| `id` | `BIGSERIAL` | primary key | Internal identifier |
| `short_code` | `TEXT` | unique, `^[0-9A-Za-z]{1,16}$` | Public code |
| `original_url` | `TEXT` | non-empty after trim | Destination |
| `is_active` | `BOOLEAN` | not null, default true | Deactivation flag |
| `created_at` | `TIMESTAMPTZ` | not null, default now | Creation time |
| `expires_at` | `TIMESTAMPTZ` | nullable | Optional expiry |
| `deactivated_at` | `TIMESTAMPTZ` | nullable | Set on delete |

Indexes:

- `urls_short_code_key`: unique lookup and collision detector.
- `urls_active_expires_at_idx`: partial index for active rows with an expiry.
  It is available to a future reaper; the current read path does not scan it.
- `urls_created_at_idx`: operational reporting and retention work.

## Invariants

- A code is unique regardless of active state. Codes are not reused.
- `is_active = false` is terminal for a row; delete does not physically remove
  it.
- A row with `expires_at <= now()` is treated as gone even if `is_active` is
  still true.
- `original_url` is validated by the application before insert. The database
  check only prevents empty values; it is not a URL parser.
- The application never stores a code outside the base62 shape accepted by the
  database constraint.

## Lifecycle

```
create -> active --(delete)--> inactive
   |
   +--(expires_at passes)--> logically expired
```

- **Create:** validate, generate code, insert, return the stored row. A unique
  violation is retried with a new code.
- **Read:** redirect resolves through Redis first; on a miss PostgreSQL returns
  the row and the application decides whether it is usable.
- **Delete:** set `is_active = false` and `deactivated_at = now()`, then write a
  Redis tombstone.
- **Expiration:** evaluated on read and during cache fill. There is no
  background reaper, so expired rows remain until an operator adds one.

## Response representation

```json
{
  "short_code": "Ab3xY9z",
  "short_url": "http://localhost:8080/Ab3xY9z",
  "original_url": "https://example.com/path",
  "created_at": "2026-10-28T02:30:00Z",
  "expires_at": null,
  "is_active": true
}
```

Timestamps are stored with timezone and rendered as RFC 3339 UTC strings.
`short_url` is derived from `PUBLIC_BASE_URL`; changing that setting does not
rewrite stored rows.

## Redis keys

| Key | Value | TTL | Purpose |
| --- | --- | --- | --- |
| `url:v1:<code>` | `P...`, `N`, or `D` | positive/negative/tombstone policy | Cache-aside entry |
| `rl:v2:<scope>:<identity>:<window>` | integer counter | two windows | Fixed-window limiter |

Redis keys are not a backup target. Losing them loses cache warmth and resets
rate-limit counters; links remain available from PostgreSQL.

## Retention

Expired and deactivated rows are retained by design. A retention job should
delete rows only after the maximum cache TTL and tombstone TTL have elapsed, or
must accept that a stale cache entry can outlive the row for a bounded period.
Any retention query should use the partial expiration index and run in bounded
batches to avoid long locks.
