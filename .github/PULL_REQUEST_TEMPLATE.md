## What this changes

<!-- What the change does, and why. If it fixes an issue, "Refs #N" - not
     "Closes #N": the person who reported it closes it, after verifying. -->

## How it was verified

<!-- Not "tests pass" - which tests, and what would have failed before.

     For a bug fix, the useful evidence is that the new test FAILS without the
     fix: revert the fix, run the test, watch it fail. Say so here.

     For a performance change, say what was measured, on which build (release,
     never debug) and with how many cores. -->

- [ ] functional tests pass with `--database mongoc`
- [ ] functional tests pass with `--database corDB`
- [ ] a test covers this change, and it fails without it
- [ ] documentation updated, if behaviour changed

## Anything a reviewer should look at first

<!-- The part you are least sure about. Saying "the locking in X" saves more
     review time than a summary of the diff. -->
