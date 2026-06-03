title: E2E latency accuracy test
status: completed
depends: t6

Steps:
- Create tests/e2e/latency.sh:
    - Start mock_server.py on port 18081 in `delay` mode (50ms fixed latency)
    - Wait for server readiness (same socket-probe loop as smoke.sh)
    - Run: wrkx -t1 -c5 -d5s -R20 -L http://localhost:18081/
    - Capture stdout; assert exit code 0
    - Assert "Latency Distribution (HdrHistogram" appears in output
    - Parse 50th-percentile value; convert to microseconds
    - Assert value is in [40000, 300000] µs (40–300ms)
- Add to Makefile test-e2e: @bash tests/e2e/latency.sh

Acceptance:
- `make test-e2e` exits 0
- 50th-percentile latency is between 40ms and 300ms
- Script fails if server returns instant responses (lower bound catches it)
