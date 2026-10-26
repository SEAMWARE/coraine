### Fixed

- Passwords and tokens no longer show in the log. They appear as `***` in mongoc's
  connection URI (`-v`, from `--dbUser`/`--dbPwd` or from `--dbURI`), in the notification and
  forwarding traces (the endpoint's `user:password` and the value of a credential header such as
  receiverInfo's `Authorization`), in the warnings that name an endpoint, in the auth plugins'
  discovery, keys and policies URLs, in the client proxy's error messages, and in a crash
  report's command line (the value after `--dbPwd`, `--dbURI` or `--troePwd`).
