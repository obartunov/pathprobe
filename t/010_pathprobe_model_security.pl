# Thin TAP wrapper for the pathprobe security model.
#
# Runs the model runner against an ALREADY-RUNNING cluster, using the standard
# PG* environment (PGHOST/PGPORT/PGUSER/PGDATABASE) and PP_PSQL for the psql
# binary.  This wrapper is intentionally NOT added to the extension Makefile's
# test wiring, so `make installcheck` (the sql/*.sql slices) is unaffected.
#
#   PP_PSQL=.../psql PGHOST=/tmp PGPORT=5432 PGUSER=postgres \
#     prove t/010_pathprobe_model_security.pl
#
# Optional: PP_MODE (smoke|pairwise|targeted|fuzz, default smoke), PP_PYTHON.
use strict;
use warnings;
use FindBin;
my $py     = $ENV{PP_PYTHON} || 'python3';
my $runner = "$FindBin::Bin/../tools/pathprobe_model_runner.py";
my $mode   = $ENV{PP_MODE} || 'smoke';
exec($py, $runner, '--only', 'security', '--mode', $mode, '--format', 'tap')
  or die "failed to exec model runner: $!";
