-- memcached_get.lua — GET workload for wrkx memcached E2E tests.
--
-- Cycles through 100 keys so the mock server sees varied keys.
-- The mock always returns a cache hit, exercising the full
-- VALUE…END parse path on every request.

local keys = {}
for i = 1, 100 do
    keys[i] = "key:" .. i
end

local idx = 0

function request()
    idx = idx + 1
    return memcached.get(keys[(idx % 100) + 1])
end
