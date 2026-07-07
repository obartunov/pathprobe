# TAP: pathprobe security model.
#
# Brings up its own node via PostgreSQL::Test::Cluster, installs the extension
# and fixtures, then runs the security model cases from PathprobeModel.pm and
# checks OK / DENIED / NO_LEAK / NO_GUC_LEAK.  The model is the source of truth.
#
# In an in-tree build this runs under `make check` / meson like any TAP test.
# To run standalone against an installed cluster, set PP_INSTALL_PATH and the
# usual PostgreSQL::Test environment (PERL5LIB, PG_REGRESS, PATH).  PP_MODE
# selects smoke (default) / pairwise / targeted / fuzz; PP_SEED sets the fuzz
# seed (default 42).
use strict;
use warnings;
use FindBin;
use lib $FindBin::Bin;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use PathprobeModel;

my $mode = $ENV{PP_MODE} || 'smoke';
my $seed = defined $ENV{PP_SEED} ? $ENV{PP_SEED} : 42;

my $node = PostgreSQL::Test::Cluster->new('pp_model_sec',
    $ENV{PP_INSTALL_PATH} ? (install_path => $ENV{PP_INSTALL_PATH}) : ());
$node->init;
$node->start;
$node->safe_psql('postgres', $PathprobeModel::SETUP_SQL);

my ($cases, $skips);
if ($mode eq 'fuzz') {
    my ($s, undef, $sk) = PathprobeModel::fuzz_sample($seed);
    ($cases, $skips) = ($s, $sk);
} else {
    ($cases, $skips) = PathprobeModel::generate_security($mode);
}

note("mode=$mode security_cases=" . scalar(@$cases)
    . " skipped=" . scalar(@$skips));
if (@$skips) {
    my $sum = PathprobeModel::skip_summary($skips);
    note(sprintf("  skip %3d  %s", $sum->{$_}, $_))
        for sort { $sum->{$b} <=> $sum->{$a} || $a cmp $b } keys %$sum;
}

for my $c (@$cases) {
    my ($ok, $fails) = PathprobeModel::run_security_case($node, $c);
    ok($ok, $c->{cid}) or diag(join("\n", @$fails));
}

$node->stop;
done_testing();
