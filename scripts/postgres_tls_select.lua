-- scripts/postgres_tls_select.lua
--
-- Smoke test for postgres+tls:// (ADR 0005, P6-3).
-- Use with:
--   ./wrkx -t2 -c10 -d5s -R50 -s scripts/postgres_tls_select.lua \
--          postgres+tls://wrkx:secret@localhost/wrkx

function request()
    return pg.query("SELECT 1")
end
