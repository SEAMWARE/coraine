### Fixed

- A context source registration subscription whose notification fails keeps its `status` (`active`, `paused`
  or `expired`, TS 104 175 § 5.2.6.5.2); the failure shows in `notification.status`, `"failed"`. A single GET
  answered the subscription's own `status` as `"failed"`, which is not one of its values; the list GET did not.
