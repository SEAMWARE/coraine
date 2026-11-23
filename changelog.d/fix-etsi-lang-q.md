### Fixed

- The `lang` filter takes a list of language ranges with quality values, as TS 104-175 § 7.2.7 (example 4)
  gives it: `lang=fr;q=0.5,nl;q=0.9` picks `nl`. Ranges are tried best q first (equal q in their order,
  `q=0` not acceptable). For each range, the key with that tag (case-insensitive) is picked, else a key the
  range is a prefix of (`de` picks `de-AT`). `*`, or no matching range, picks the default
  (`@none` > `en` > the first key). Before, `lang` was compared as one whole tag.
