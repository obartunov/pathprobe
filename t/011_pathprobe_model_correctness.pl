# TAP: pathprobe correctness model.  See t/010_pathprobe_model_security.pl for
# the environment and mode notes.  Checks the path/hypo/advisor lifecycle
# invariants and allowed fate sets from PathprobeModel.pm.
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

my $node = PostgreSQL::Test::Cluster->new('pp_model_cor',
    $ENV{PP_INSTALL_PATH} ? (install_path => $ENV{PP_INSTALL_PATH}) : ());
$node->init;
$node->start;
$node->safe_psql('postgres', $PathprobeModel::SETUP_SQL);

my $cases;
if ($mode eq 'fuzz') {
    my (undef, $c, undef) = PathprobeModel::fuzz_sample($seed);
    $cases = $c;
} else {
    $cases = PathprobeModel::generate_correctness($mode);
}

note("mode=$mode correctness_cases=" . scalar(@$cases));

for my $c (@$cases) {
    my ($ok, $fails) = PathprobeModel::run_correctness_case($node, $c);
    ok($ok, $c->{cid}) or diag(join("\n", @$fails));
}

$node->stop;
done_testing();
