# Applying the pathprobe core hooks patch

```text
target:       PostgreSQL 20devel
baseline:     3be823486f2c9f405fc754ac0ece3ce412aee105 (actual tested baseline)
build define: -DHAVE_PATHPROBE_HOOKS   (add to COPT/CFLAGS for the extension;
                                        stock builds work without the patch,
                                        with reduced probe coverage)
```

```bash
cd postgresql
git checkout 3be8234
git apply pathprobe-core-hooks.patch
./configure ... && make install
```

The extension auto-detects hook availability (stock/patched tiering); the
patch adds the add_path/rel-level observation hooks used by probe1 and
the proposed-path frontier check, and the build_index_paths() decision hook
used for hypothetical-index fate.  The latter is detected separately through
HAVE_PATHPROBE_INDEX_PATH_HOOK, so a server built with an older copy of the
patch still works, without index-build coverage.  Likewise
HAVE_PATHPROBE_REJECTED_DOMINATOR says that the rejected hook gets the old
path that dominated the new one, not the rel's cheapest total path; without
it REJECTED events carry no dominator.  HAVE_PATHPROBE_REMOVED_HOOK says that
add_path() reports the old paths it removed before rejecting the new path;
without it pathprobe cannot drop their path ids (see json-format.md).
