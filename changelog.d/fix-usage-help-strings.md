### Fixed

- `coraine -u` describes its options as [Command-line options](doc/options.md) does. Wrong before:
  `--auth` named a `none` value there is none of, `--distOpTimeout` claimed notifications and @context
  downloads (10 s each, not it), `--insecureNotif` only notifications and forwards (it is every outgoing
  HTTPS request, `mqtts://` too), `--httpLoops` "built-in server only" (the cor:// listener too),
  `--connectionPoolSize` "MHD thread pool" (the workers, on both servers), `--ddsSyncTimeout` left out the
  action goals' wait (504), `--asyncSnapshot` promised a status in the 201 (no body either way),
  `--fdscOrganization` showed no default (it has the PAP's); `--bridgeConfig`, `--contextSourceExtras`,
  `--subStatsFlushInterval`, `--cooldownMillis`, `--high-availability`, `--dbName`, `--dbUser`, `--dbPwd`,
  `--dbURI`, `--troeName`, `--fdscPolicies`, `--fdscOwnerMaxAge` and `--traceLevels` say their defaults
  and limits. The high-availability errors name the option as `-ha` (they said `--ha`, which is no option).
