### Fixed

- Retrieve Entity with `join=flat` answers an array of Entities also when no linked entity is added (no
  Relationships, all targets in `containedBy`, a self-cycle); it was the bare Entity object. TS 104-175 § 7.7.3:
  a flat retrieval "shall always consist of an array of Entities".
