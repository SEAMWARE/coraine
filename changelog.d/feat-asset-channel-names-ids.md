### Changed behaviour

- AssetChannels: an attribute name's first word is lower-case with a leading acronym whole - `URIOfTheProduct`
  is `uriOfTheProduct`, `AASReference` `aasReference`, `PLC` `plc` (was `uRIOfTheProduct`, `aASReference`,
  `pLC`). Names that start with one capital are as they were.
- AssetChannels: a model in which two entity nodes have the same id (an AAS shell and a submodel given one id)
  is refused - nothing written, the AssetChannel dormant with the id in `statusReason`, `samplesRefused`
  counting it - and taken again with the source's next model of distinct ids. Before, the two were written as
  one entity of both types.
