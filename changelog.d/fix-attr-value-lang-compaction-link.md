### Fixed

- `GET /entities/{id}/attrs/{attrId}/value` (the value only Attribute fragment): a JSON object value, and an
  object inside an array value, came back with its member names expanded
  (`"https://uri.etsi.org/ngsi-ld/default-context/streetAddress"`); they are compacted with the request's
  `@context`, as in `GET .../attrs/{attrId}`. `?lang` was ignored - a LanguageProperty gave its whole languageMap;
  it now gives the string in that language (§ 7.2.7). And the response had no Link header: a value only fragment
  cannot hold an `@context`, so it is in the Link header (TS 104-175 § 5.4.2). Found by the ETSI suite's 062_02.
