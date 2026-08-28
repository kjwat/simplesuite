# Contributing

Small bug fixes, portability fixes, documentation corrections, and narrowly
scoped build improvements are appropriate. Please keep existing interfaces and
keybindings stable unless a change has been discussed first.

Before submitting a change, run:

```sh
make release-simplewords
```

`./build.sh` runs the same release gate before installing. The gate requires a
warning-free build, the full test suite, SimpleWords ASan/UBSan and real-PTY
checks, persistence fault injection, and destructive-path coverage. Do not
bypass it for a release or image build.

Avoid generated binaries, local configuration, caches, logs, and unrelated
personal files in commits. Large refactors, new frameworks, and dependency-heavy
rewrites are outside the project's current scope.
