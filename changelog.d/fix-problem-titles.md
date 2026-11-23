### Changed behaviour

- A ProblemDetails `title` now says what failed instead of repeating the HTTP status phrase: "Entity not
  found", "Snapshot not found", "Inside an exclusive registration", "Operation not supported by the
  registration", "Forward failed", "Not supported by the database", ... (about 200 sites; TS 104-175
  § 8.3.3 asks for "a short string summarizing the error"). `type`, `status` and `detail` are unchanged.
  An `about:blank` error (the policy API, authentication) keeps the status phrase, as RFC 9457 § 4.2.1
  asks, and so does a coraine type that names the phrase itself (MethodNotAllowed, NotImplemented).
