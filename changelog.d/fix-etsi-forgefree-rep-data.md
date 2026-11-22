### Fixed

- The nightly ETSI conformance job takes the suite's `@context` from the local context server again, not from
  forge.etsi.org: `test/etsi/etsiForgeFree.sh` rewrites the suite's forge URLs of any forge group - it knew only
  `rep/cim`, and the suite moved to `rep/data`, so nothing was rewritten and the run depended on forge being up.
  The job clones the suite from `rep/data`.
