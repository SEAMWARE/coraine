# changelog.d - the entries of the coming release

A pull request with a change a user would notice adds its [CHANGELOG](../CHANGELOG.md) entry here, as a file
of its own - it does not edit `CHANGELOG.md`. Each entry is its own file, so merging one pull request does
not put the others in conflict.

**The file:** `changelog.d/<name>.md`. The name is the branch name with `/` written as `-`
(`fix-cli-required-option.md`), with no spaces. The content is one or more sections, each a `### ` heading
followed by `- ` bullets, written as `CHANGELOG.md`'s are:

    ### Fixed

    - `coraine --port` without a value names the option and stops the start - it took the next option
      as the port's value.
      ([Command-line options](doc/options.md#--port))

**The sections**, in the order a release lists them: `New`, `Faster`, `Less memory`, `Changed behaviour`,
`Fixed`. A deprecation goes under `Changed behaviour` and names the release that refuses the option. Links
are written relative to the repository root, as in `CHANGELOG.md`, where the entry ends up.

**What happens to it:** `tools/changelog.sh` prints the coming release's section - `CHANGELOG.md`'s
unreleased part with these entries added, each in the order it reached main. At a release,
`tools/changelog.sh --release <version>` writes that section into `CHANGELOG.md` and removes the files.
CI checks the files' form on every pull request (`tools/changelog.sh --check`).
