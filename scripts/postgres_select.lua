-- scripts/postgres_select.lua
--
-- Basic PostgreSQL workload for wrkx (ADR 0005, P6-1).
-- Issues a simple SELECT against a counter key.
-- Use with:
--   ./wrkx -t4 -c100 -d10s -R500 -s scripts/postgres_select.lua \
--          postgres://wrkx:secret@localhost/wrkx

local counter = 0

function request()
    counter = counter + 1
    local key = counter % 100
    return pg.query(string.format("SELECT %d", key))
end
