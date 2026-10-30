### New

- `COR_OPTIMIZE=speed|size` (`coraine-build --optimize`): a build whose defaults spend RAM rather than
  throughput, for small hardware at low request rates - a worker per CPU whatever the store, and one
  malloc arena for the process. With mongoc on 4 CPUs the broker holds 25 MB after 100 000 entities
  created in batches of 1000, where the default build holds 197 MB. `speed` stays the default;
  `coraine --version` and `GET /build` say which a binary is.
  ([COR_OPTIMIZE](doc/building-details.md#cor_optimize))
