// scripts/http_basic.js — Minimal HTTP/1.1 GET workload for the QuickJS engine.
//
// Usage:
//   ./wrkx -t2 -c10 -d5s -R100 --engine=quickjs -s scripts/http_basic.js http://localhost
//
// The request() function is called once per request.  wrk.host and wrk.path
// are populated by the engine after --configure() runs, so they reflect the
// target URL supplied on the command line.

function request() {
    var host = (wrk && wrk.host) ? wrk.host : "localhost";
    var path = (wrk && wrk.path) ? wrk.path : "/";
    return "GET " + path + " HTTP/1.1\r\n" +
           "Host: " + host + "\r\n" +
           "Connection: keep-alive\r\n" +
           "\r\n";
}
