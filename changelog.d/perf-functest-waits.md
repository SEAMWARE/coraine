### Changed

- The functional-test harness checks a broker's port without starting a process per check (`corPortOpen`
  was a `bash -c` each time, 2-3 ms): a broker that is up is seen sooner, about 8 ms per test.
