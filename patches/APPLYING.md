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
the proposed-path frontier check.
