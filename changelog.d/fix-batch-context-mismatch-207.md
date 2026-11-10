- A batch entity breaking the `@context` / Content-Type rule (TS 104 176 § 6.2.4) - no `@context` under
  `application/ld+json`, one under `application/json` - is that entity's BadRequestData in the batch's
  `errors[]` (207), and the other entities are processed. Was a 400 for the whole request. Decided at the ETSI
  NGSI-LD API call of 2026-11-10 (ETSI suite 003_06_01, 003_08_01). corNgsild#122.
