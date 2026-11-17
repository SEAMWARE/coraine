### Fixed

- A subscription whose notifications the auth plugin drops whole - its owner not kept, or no longer allowed
  (`--oidcOwnerMaxAge` run out, no read right, another tenant) - is said in the log: a warning naming the
  subscription, the reason and the remedy (a PATCH with a valid token), at most once an hour a subscription.
  Until now the drop was silent: no failure on the subscription, no log line.
