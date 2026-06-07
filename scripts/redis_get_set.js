// scripts/redis_get_set.js — Redis GET/SET workload for the QuickJS engine.
// Equivalent of scripts/redis_get_set.lua (ADR 0005, Phase 5, t075).
//
// Usage:
//   ./wrkx -t4 -c100 -d10s -R1000 --engine=quickjs \
//          -s scripts/redis_get_set.js redis://localhost:6379
//
// Alternates GET and SET across 100 keys so both command paths are exercised.
// The redis.get() / redis.set() helpers are registered by the Redis extension
// (extensions/redis/redis_quickjs_helpers.c) and return raw RESP frame bytes
// ready to be written to the wire.

var counter = 0;

function request() {
    counter++;
    var key = "wrkx:key:" + (counter % 100);
    return (counter % 2 === 0) ? redis.set(key, "value") : redis.get(key);
}
