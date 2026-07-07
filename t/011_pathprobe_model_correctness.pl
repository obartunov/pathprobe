# Thin TAP wrapper for the pathprobe correctness model.  See the header of
# t/010_pathprobe_model_security.pl for connection and scope notes.  Not wired
# into `make installcheck`.
use strict;
use warnings;
use FindBin;
my $py     = $ENV{PP_PYTHON} || 'python3';
my $runner = "$FindBin::Bin/../tools/pathprobe_model_runner.py";
my $mode   = $ENV{PP_MODE} || 'smoke';
exec($py, $runner, '--only', 'correctness', '--mode', $mode, '--format', 'tap')
  or die "failed to exec model runner: $!";
