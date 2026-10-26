### Fixed

- A request body nested deeper than 100 levels (arrays and objects) is refused with 400 `InvalidRequest`,
  detail `request body nested deeper than 100 levels`, and the broker serves the next request - a body of
  enough nested brackets, well under `--maxRequestSize`, stopped the broker.
  ([Request size](doc/installation.md#request-size))
