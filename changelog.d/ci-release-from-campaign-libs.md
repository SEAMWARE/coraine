### Changed behaviour

- A release is built from the libraries its pre-release campaign tested, not from each library's `main`
  at tag time: the campaign records the commit of every library (corLibs and c³ too) once, every part of
  it builds from that record, and `release.yml` builds and tags the release from the newest green
  campaign of the tag's tree - with none, it stops before publishing anything. c³'s tag must be the
  commit in the record. 0.6.0 was one commit newer in the NGSI-LD library than its campaign.
  ([The pre-release campaign](doc/testing.md#the-pre-release-campaign))
