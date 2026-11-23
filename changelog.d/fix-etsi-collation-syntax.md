### Fixed

- A `collation` that is not a well-formed BCP 47 language tag (RFC 6067) is 400 BadRequestData
  (TS 104-175 § 10.4.3.4), in every build. Before, `collation=!invalid!` was accepted and the default
  order was used. A well-formed tag of an unknown language still falls back to the root order.
