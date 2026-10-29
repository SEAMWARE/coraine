### Fixed

- A batch that names one Entity more than once - `POST /ngsi-ld/v1/entityOperations/upsert?options=update`,
  `/entityOperations/update` and `/entityOperations/merge` - records each entry in the temporal history with
  its own value and `observedAt`. Every instance carried the last entry's values, so a batch of monthly
  readings stored copies of the last one; Batch Merge's notification entries did the same. With
  `--troe corDB` only one instance was recorded, the replace-mode upsert's too.
