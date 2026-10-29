BEGIN;

CREATE TABLE IF NOT EXISTS urls (
    id             BIGSERIAL PRIMARY KEY,
    short_code     TEXT NOT NULL,
    original_url   TEXT NOT NULL,
    is_active      BOOLEAN NOT NULL DEFAULT TRUE,
    created_at     TIMESTAMPTZ NOT NULL DEFAULT now(),
    expires_at     TIMESTAMPTZ,
    deactivated_at TIMESTAMPTZ,
    CONSTRAINT urls_short_code_key UNIQUE (short_code),
    CONSTRAINT urls_short_code_format CHECK (short_code ~ '^[0-9A-Za-z]{1,16}$'),
    CONSTRAINT urls_original_url_present CHECK length(trim(original_url)) > 0
);

CREATE INDEX IF NOT EXISTS urls_active_expires_at_idx
    ON urls (expires_at)
    WHERE is_active AND expires_at IS NOT NULL;

CREATE INDEX IF NOT EXISTS urls_created_at_idx
    ON urls (created_at);

COMMIT;
