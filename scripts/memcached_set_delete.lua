-- memcached_set_delete.lua — set/get/delete workload for wrkx E2E tests.
--
-- Cycles four operations in order: set → get → delete → get.
-- The mock server always responds with STORED / VALUE…END / DELETED,
-- so all four legs exercise their respective encode/parse paths.

local keys = {}
for i = 1, 100 do
    keys[i] = "key:" .. i
end

local counter = 0

function request()
    counter = counter + 1
    local key   = keys[(counter % 100) + 1]
    local phase = (counter - 1) % 4
    if phase == 0 then
        return memcached.set(key, "value")
    elseif phase == 1 then
        return memcached.get(key)
    elseif phase == 2 then
        return memcached.delete(key)
    else
        return memcached.get(key)
    end
end
