TODO wrkx

Bugs:
- strange behavoir on large connection parameters
./wrkx -c8 -t8 -d20 -L -R10000  http://localhost
will make ~10000 requests per second, as requested
./wrkx -c500 -t8 -d20 -L -R10000  http://localhost
will make ~4800 requests per second
./baseline/wrkx0 -c500 -t8 -d20 -L -R10000  http://localhost
will also make ~4800 requests per second but this is strange as -c500 must be cheap and lead to 10000 RPS as well. 
Investigate the issue
- baseline maybe restore 'main' or 'pre-phase0' version and build again?
- provide the nginx.config maybe that's the promlem. 

Backlog:
• REDIS and other: basic parameters, methods get/set, key ranges, key size
• output beautify, separate sections, animation like in npm/brew, configuration: ./configure string "as is" + extensions enabled, input: write back all params, results: calibration, latency summary, latency spectrum, final summary with important notes (please pay attention to + next steps)
• latency vs u-latency summary and notes
• pseudo-graphical graph-style representation of latency spectrum
• more debug-like counters like reconnects, keepalive or not keepalive, ae/even engine counters (events)
• json-like output to use in automation
• human-readable summary on anomalies
• other areas of improvements: performance, more protocol extensions, CI improvements. Gates A-? - agent wanted to refactor this piece. 
