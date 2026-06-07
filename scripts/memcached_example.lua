-- scripts/memcached_example.lua
--
-- Example memcached workload for wrkx (ADR 0005, Phase 4).
--
-- Mixed GET / SET workload over a pool of 200 keys.  Two-thirds of
-- requests are GETs (cache-hit dominated); one-third are SETs (simulate
-- cache warming / invalidation).
--
-- Usage:
--   ./wrkx -t4 -c100 -d10s -R1000 \
--          -s scripts/memcached_example.lua \
--          memcached://localhost:11211
--
-- Build with memcached extension:
--   make EXTENSIONS=memcached

local N       = 200        -- key-space size
local counter = 0

function request()
    counter = counter + 1
    local key = "wrkx:" .. (counter % N)
    if counter % 3 == 0 then
        return memcached.set(key, "value")
    else
        return memcached.get(key)
    end
end
