-- memcached_counters.lua — incr/decr counter workload for wrkx E2E tests.
--
-- Cycles three operations per key: set → incr → decr.
-- Each Lua state (one per thread) starts at phase 0 so the first request
-- from every connection is always SET, guaranteeing the key exists before
-- any INCR or DECR is issued on that connection.

local phase = 0

function request()
    phase = phase + 1
    local key = "cnt:" .. ((phase % 10) + 1)
    local p   = (phase - 1) % 3
    if p == 0 then
        return memcached.set(key, "100")
    elseif p == 1 then
        return memcached.incr(key)
    else
        return memcached.decr(key, 5)
    end
end
