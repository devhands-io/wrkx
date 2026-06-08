-- scripts/postgres_prepared.lua
--
-- PostgreSQL prepared-statement workload for wrkx (ADR 0005, P6-2).
-- Uses the extended query protocol: Parse + Bind + Describe('P') + Execute + Sync per request.
-- Use with:
--   ./wrkx -t4 -c100 -d10s -R500 -s scripts/postgres_prepared.lua \
--          postgres://wrkx:secret@localhost/wrkx

local counter = 0
local stmt = pg.prepare("SELECT $1::int")

function request()
    counter = counter + 1
    return pg.execute(stmt, tostring(counter % 1000))
end
