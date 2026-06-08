-- scripts/mysql_select.lua
--
-- Basic MySQL workload for wrkx (ADR 0005, P6-4).
-- Issues a simple SELECT against a counter value.
-- Use with:
--   ./wrkx -t4 -c100 -d10s -R500 -s scripts/mysql_select.lua \
--          mysql://wrkx:secret@localhost/wrkx

local counter = 0

function request()
    counter = counter + 1
    local key = counter % 100
    return mysql.query(string.format("SELECT %d", key))
end
