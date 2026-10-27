### Fixed

- A request body's numbers follow JSON's grammar: `+5`, `1.`, a bare `-` and `1.e5` are refused with 400 -
  they were taken as 5, 1, 0 and 100000. `1e400` is refused - it was stored as infinity and rendered `inf`,
  which made every response holding it invalid JSON.
- An integer past 64 bits (`12345678901234567890`) is stored as a float - it wrapped to a negative integer.
- A float is stored as the double nearest to its text: `q=a==0.3` did not find a stored `0.3` on mongoc
  (stored as 0.30000000000000004), and decimals past the 12th were dropped.
  ([Request body](doc/installation.md#request-size))
