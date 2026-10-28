### New

- `expandValues`, `jsonKeys` and `langProperties` in a POST Query body and a Subscription, and `aggrMethods`
  in a temporal Query's `temporalQ`: a string, comma separated, or an array of strings with one name per
  item - an item holding a comma, an empty item or a non-string is a 400. A name is short or already
  expanded. A Subscription keeps the form it was sent in. The URL parameters stay comma separated lists.
  ([API walkthrough](doc/api-walkthrough.md#3-query))
