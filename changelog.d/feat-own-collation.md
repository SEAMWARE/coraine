### Changed behaviour

- `orderBy` on strings without ICU (the default build) follows the root collation (TS 104-175 § 7.6.2.1)
  instead of an ASCII approximation: letters with accents sort beside their base letter (`äpple` beside
  `apple`), punctuation before digits, case only breaks ties. `collation=` now works without ICU for `sv`,
  `de-u-co-phonebk` and the numeric key `-u-kn` (`item2` < `item10`); any other tag is the root order. The
  order is ICU 78's for every Latin-1 and Latin Extended-A letter; other scripts sort after Latin, by code
  point. An ICU build (`make COR_WITH_ICU=1`) still uses ICU.
