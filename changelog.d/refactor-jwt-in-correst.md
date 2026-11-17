### Changed

- The access-token check of `--auth fdsc` and `--auth oidc` (a JWT, RS256 / ES256, against a JWK Set; the
  tokens checked already; the JWK Set fetched) is corRest's now (`corRestJwt*`), no longer a copy inside each
  auth plugin - the same code, so corCep can check tokens with it too. No change in behaviour.
