### Fixed

- Simplified temporal representation (`options=temporalValues`): a deleted ListProperty or ListRelationship
  gives `["urn:ngsi-ld:null"]` as the list of its `valueLists` / `objectLists` pair, as TS 104-175 § 5.3.2.6
  requires an array there. Before, it gave the bare string `"urn:ngsi-ld:null"`.
