title: Mock HTTP server + smoke E2E test
status: completed
depends: t1

Steps:
- Create tests/e2e/mock_server.py:
    - mode 1: instant 200 OK (smoke)
    - mode 2: fixed 50ms delay before response
    - mode 3: 10% of responses return 503
    - mode 4: close connection abruptly (tests reconnect patch)
    - port passed as argv[1], mode as argv[2]
- Create tests/e2e/smoke.sh:
    python3 mock_server.py 18080 instant &
    ./wrk -t1 -c5 -d3s -R20 http://localhost:18080/
    assert exit code 0
    kill mock server
- Update Makefile test-e2e to call smoke.sh

Acceptance:
- `make test-e2e` exits 0
- wrk connects, completes requests, exits cleanly
