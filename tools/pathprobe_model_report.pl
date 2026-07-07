#!/usr/bin/env perl
# Offline model reporting for the pathprobe model-based tests (no database).
# Prints the coverage matrix, case list, skip reasons, or per-case debug SQL,
# using the same model as the TAP tests.  SQL is debug/export only; it is not a
# committed test artifact.
#
#   perl tools/pathprobe_model_report.pl [--mode smoke|pairwise|targeted] ACTION
#   ACTION := matrix (default) | list | skips | emit-sql
use strict;
use warnings;
use FindBin;
use lib "$FindBin::Bin/../t";
use PathprobeModel;

my $mode = 'pairwise';
my @args;
while (my $a = shift @ARGV) {
    if    ($a eq '--mode') { $mode = shift @ARGV; }
    else                   { push @args, $a; }
}
my $action = $args[0] // 'matrix';

my ($sec, $skips) = PathprobeModel::generate_security($mode);
my $cor = PathprobeModel::generate_correctness($mode);

if ($action eq 'list') {
    printf "# security cases (%d)\n", scalar(@$sec);
    printf "  %-52s role=%-20s expect=%s\n", $_->{cid}, $_->{role},
        join(',', sort keys %{$_->{expect}}) for @$sec;
    printf "# correctness cases (%d)\n", scalar(@$cor);
    for my $c (@$cor) {
        my $f = $c->{allowed_fates} ? join(',', sort keys %{$c->{allowed_fates}}) : '-';
        printf "  %-52s par=%-10s fates=%s\n", $c->{cid}, $c->{parallelism}, $f;
    }
    exit 0;
}
if ($action eq 'skips') {
    my $sum = PathprobeModel::skip_summary($skips);
    printf "# skipped combinations (%d) by reason\n", scalar(@$skips);
    printf "  %3d  %s\n", $sum->{$_}, $_
        for sort { $sum->{$b} <=> $sum->{$a} || $a cmp $b } keys %$sum;
    exit 0;
}
if ($action eq 'emit-sql') {
    for my $c (@$sec) {
        printf "-- SECURITY %s (expect %s)\n%s\n", $c->{cid},
            join(',', sort keys %{$c->{expect}}), PathprobeModel::build_security_sql($c);
    }
    for my $c (@$cor) {
        printf "-- CORRECTNESS %s\n%s\n", $c->{cid}, PathprobeModel::build_correctness_sql($c);
    }
    exit 0;
}

# matrix
print "# coverage matrix (security)\n";
my %DIM = (
    role         => \@PathprobeModel::ROLES,
    relation     => \@PathprobeModel::RELATIONS,
    query_shape  => \@PathprobeModel::QUERY_SHAPES,
    spec_shape   => \@PathprobeModel::SPEC_SHAPES,
);
for my $dim (qw(role relation query_shape spec_shape)) {
    my %count = map { $_ => 0 } @{ $DIM{$dim} };
    for my $c (@$sec) {
        my $v = $dim eq 'role' ? $c->{role} : $c->{dims}{$dim};
        $count{$v}++ if defined $v && exists $count{$v};
    }
    printf "  %-12s %s\n", $dim,
        join(', ', map { "$_=$count{$_}" } @{ $DIM{$dim} });
}
print "# coverage matrix (correctness)\n";
my %CDIM = (parallelism => \@PathprobeModel::PARALLELISM, policy => \@PathprobeModel::POLICIES);
for my $dim (qw(parallelism policy)) {
    my %count = map { $_ => 0 } @{ $CDIM{$dim} };
    for my $c (@$cor) { my $v = $c->{dims}{$dim}; $count{$v}++ if defined $v && exists $count{$v}; }
    printf "  %-12s %s\n", $dim, join(', ', map { "$_=$count{$_}" } @{ $CDIM{$dim} });
}
if (@$skips) {
    my $sum = PathprobeModel::skip_summary($skips);
    printf "# skipped (%d) by reason\n", scalar(@$skips);
    printf "  %3d  %s\n", $sum->{$_}, $_
        for sort { $sum->{$b} <=> $sum->{$a} || $a cmp $b } keys %$sum;
}
