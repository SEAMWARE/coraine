### New

- A registration PATCHed: the subordinate subscriptions of the matching subscriptions follow it (§ 10.5.2.4) -
  PATCHed at the Context Source when the registration's change narrows their entities differently, deleted when
  it no longer matches (another type, no `createSubscription`), deleted at the old endpoint and created at the new
  one when its endpoint or tenant changes, and created when it matches now. Creation and deletion of a
  registration were handled already.
  ([`--distributed`](doc/options.md#--distributed))

### Fixed

- A broker without `--distributed` created subordinate subscriptions at a Context Source when a registration was
  created after a matching subscription; it creates none.
