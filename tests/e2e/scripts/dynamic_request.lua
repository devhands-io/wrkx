-- tests/e2e/scripts/dynamic_request.lua
--
-- Exercises the dynamic request() path: script_is_static() returns false
-- when a global request() function is defined, causing wrkx to call it
-- per-request rather than reusing a pre-built string.

local paths = {"/", "/health", "/index"}
local i = 0

function request()
    i = (i % #paths) + 1
    return wrk.format("GET", paths[i])
end
