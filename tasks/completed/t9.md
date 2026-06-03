title: Add -l option for Latency Distribution without Detailed Percentile spectrum
status: completed
depends: []

Steps:
- Add a new -l (lowercase L) CLI option in wrk.c / argument parsing
- When -l is set, calculate and print only the "Latency Distribution
  (HdrHistogram - Recorded Latency)" section
- When -l is set, do NOT print the "Detailed Percentile spectrum" section
- The existing -L option behaviour must remain unchanged
- Update the usage/help output (printed when binary is called without
  arguments or with wrong parameters) to document the new -l option

Acceptance:
- ./wrkx -l ... prints "Latency Distribution" section and no "Detailed Percentile spectrum"
- ./wrkx -L ... still prints both sections (unchanged behaviour)
- ./wrkx (no args) usage text mentions the -l option
- `make test` passes, 0 failures
