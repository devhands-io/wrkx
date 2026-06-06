-- scripts/redis_get_set.lua
--
-- Example Redis workload for wrkx (ADR 0005, P2-3).
--
-- Alternates between GET and SET on a pool of 100 keys.
-- Use with:
--   ./wrkx -t4 -c100 -d10s -R1000 -s scripts/redis_get_set.lua redis://localhost:6379

local counter = 0

function request()
    counter = counter + 1
    local key = "key:" .. math.random(1, 100)
    if counter % 2 == 0 then
        return redis.command("SET", key, "value")
    else
        return redis.command("GET", key)
    end
end
