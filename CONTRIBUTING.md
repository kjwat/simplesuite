# Contributing

Small bug fixes, portability fixes, documentation corrections, and narrowly
scoped build improvements are appropriate. Please keep existing interfaces and
keybindings stable unless a change has been discussed first.

Before submitting a change, run:

```sh
make clean
make
```

Avoid generated binaries, local configuration, caches, logs, and unrelated
personal files in commits. Large refactors, new frameworks, and dependency-heavy
rewrites are outside the project's current scope.
