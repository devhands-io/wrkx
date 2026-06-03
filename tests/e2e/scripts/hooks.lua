-- tests/e2e/scripts/hooks.lua
--
-- Exercises all Lua hook functions in a real wrkx run.
--
-- Architecture note: setup() is called on the MAIN Lua state (same state
-- that done() runs in), so setup_called is trackable from done().
-- init() and response() run on PER-THREAD Lua states; they cannot share
-- local variables with the main state, so they are verified by absence of
-- crash rather than by a counter.

local setup_called = false

function setup(thread)
    setup_called = true
end

function init(args)
    -- called on per-thread state; verified by not crashing
end

function response(status, headers, body)
    -- called on per-thread state; verified by not crashing
end

function done(summary, latency, requests)
    io.write("HOOK:setup=" .. tostring(setup_called) .. "\n")
    io.write("HOOK:done=true\n")
end
