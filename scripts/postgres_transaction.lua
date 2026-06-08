-- scripts/postgres_transaction.lua
--
-- PostgreSQL transaction workload for wrkx (ADR 0005, P6-3).
-- Issues BEGIN + INSERT + COMMIT as a single pipelined request.
-- Use with:
--   ./wrkx -t4 -c100 -d10s -R500 -s scripts/postgres_transaction.lua \
--          postgres+tls://wrkx:secret@localhost/wrkx

local counter = 0

function request()
    counter = counter + 1
    local id  = counter % 10000
    local val = tostring(id * 7)
    return pg.begin()
        .. pg.execute("INSERT INTO bench(id, val) VALUES($1, $2)",
                       tostring(id), val)
        .. pg.commit()
end

function response(status, _, _)
    if status ~= 0 then return end
    local r = pg.result()
    if r and r.status == "E" then
        -- transaction rolled back; count but don't crash
    end
end
