### Changed behaviour

- The nightly ETSI run accepts the test purposes listed in `test/etsi/deviations.txt`: a listed test that fails
  is named in the summary as a deliberate deviation and does not fail the job, and one that passes is named
  for removal from the list. The list holds the three test purposes that delete the scope with
  `DELETE /entities/{id}/attrs/scope` (013_04, 020_19_01, 020_20_01), which coraine refuses (400).
