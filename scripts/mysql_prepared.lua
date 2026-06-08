-- scripts/mysql_prepared.lua
--
-- MySQL prepared-statement workload for wrkx (ADR 0005, P6-5).
-- Demonstrates per-request parameterization via mysql.execute().
-- Use with:
--   ./wrkx -t4 -c100 -d10s -R500 -s scripts/mysql_prepared.lua \
--          mysql://wrkx:secret@localhost/wrkx

local stmt = mysql.prepare("SELECT ?")
local counter = 0

function request()
    counter = counter + 1
    return mysql.execute(stmt, tostring(counter % 1000))
end
