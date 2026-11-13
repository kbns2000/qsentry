# The changes of open62541-qnx

Each `.change` file in this directory is one change to the official
single-file release of open62541 v1.4.10 (`open62541.c`, `open62541.h`).
`scripts/port.py` applies them in the order of [`series`](series) and stops if
a text it replaces is not found exactly once, so a different upstream version
fails loudly instead of producing a half-ported build.

A change file starts with a header:

```
# open62541-qnx change: <id>
# title: <one line>
# file: open62541.c | open62541.h          (default for the hunks)
# upstream: <the source file(s) of open62541 that the hunks belong to>
# test: <the on-target tests that show the change>
#
# <why: the problem on QNX, and what the change does>
```

followed by hunks: the text of the release, then the text of the port.

```
@@ hunk [open62541.c|open62541.h]
<<<<<<< open62541 v1.4.10
(text of the release)
=======
(text of the port)
>>>>>>> open62541-qnx
```

Every line that the port adds carries a `qnx:` comment, so the changes can
also be found in the generated `third_party/open62541.c`.

`python scripts/port.py --list` prints the list. The tests named in `# test:`
run on the target twice: in `o6q_check`, built from the port, and in
`o6q_check_upstream`, built from the release with the same build
configuration and no change in behaviour. `scripts/o6q.py report` puts the
two results side by side for every change.

## License

The changes modify open62541, which is licensed under the Mozilla Public
License 2.0, and are licensed under the same terms: see
[`LICENSE-MPL-2.0`](LICENSE-MPL-2.0). The rest of this repository (scripts,
tests, demo server, documentation) is licensed under Apache-2.0.
