# Release / import checklist

Short checklist for landing the pathprobe import.

```text
build:        make PG_CONFIG=<pg_config>
install:      make PG_CONFIG=<pg_config> install
regression:   make PG_CONFIG=<pg_config> installcheck   (expects all slices green)
core hooks:   see patches/APPLYING.md (target 20devel, -DHAVE_PATHPROBE_HOOKS)
```

Import history is layered and bisectable: core patch artifact,
observability, propose/advisor/hypo, tools, tests, docs.  Each commit
builds, installs, and passes its own installcheck slice.

Security boundary before enabling propose/advisor for untrusted users:
see README "Security status" and docs/technical/safety-and-limits.md.
