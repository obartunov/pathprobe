package PathprobeModel;

# Source of truth for the pathprobe model-based tests (native Perl/TAP port).
# Declares the model dimensions, fixtures, explicit cases, the rule-based
# oracle, and the bounded generators.  The TAP tests t/010 and t/011 consume
# this module; there is no Python dependency and no committed generated-SQL wall
# (SQL is emitted only on request).

use strict;
use warnings;
use JSON::PP;

my $JSON = JSON::PP->new->canonical(1)->allow_nonref(1);

# ---- dimensions -----------------------------------------------------------

our @ROLES = qw(
    superuser table_owner agent_table_select agent_column_select
    agent_no_select agent_no_schema_usage intruder
);
our @RELATIONS = qw(
    allowed_table private_table column_restricted_table rls_table
    view_over_allowed_table view_over_private_table
);
our @QUERY_SHAPES = qw(
    top_select cte subquery exists_sublink union_branch join
    multi_statement dml_or_modifying_cte
);
our @SPEC_SHAPES = qw(
    none candidate_allowed_rel candidate_private_rel
    structured_hypo_allowed_column structured_hypo_forbidden_column
    structured_hypo_mixed_columns hypo_sql advisor_true hints_valid
    hints_malformed
);
our @PARALLELISM = qw(pinned_off default forced_on);
our @POLICIES    = qw(winner_forbidden_true winner_forbidden_false);
our %FATE_ALPHABET = map { $_ => 1 } qw(
    target_not_found not_examined unknown rejected unknown_after_accept
    survived_at_point survived displaced survived_upper
);

# role dimension -> concrete SQL role (undef => RESET ROLE / superuser)
our %ROLE_SQL = (
    superuser             => undef,
    table_owner           => 'ppm_owner',
    agent_table_select    => 'ppm_agent_tbl',
    agent_column_select   => 'ppm_agent_col',
    agent_no_select       => 'ppm_agent_none',
    agent_no_schema_usage => 'ppm_agent_noschema',
    intruder              => 'ppm_intruder',
);

# ---- JSON spec builders ---------------------------------------------------

# _cand_spec(total_cost => .., rows => 1, winner_forbidden => 0,
#            relname => 'allowed', pathname => 'c', provider => 'model',
#            pathkeys => [..], parallel_safe => 0|1)
sub _cand_spec {
    my %o = @_;
    $o{rows}     //= 1;
    $o{relname}  //= 'allowed';
    $o{pathname} //= 'c';
    $o{provider} //= 'model';
    my %spec = (
        target    => { relname => $o{relname} },
        candidate => { pathname => $o{pathname}, provider => $o{provider} },
        cost      => { total_cost => $o{total_cost} + 0.0, rows => $o{rows} + 0 },
    );
    $spec{mode} = { winner_forbidden => JSON::PP::true } if $o{winner_forbidden};
    my %planner;
    $planner{pathkeys} = $o{pathkeys} if $o{pathkeys};
    $planner{parallel_safe} = $o{parallel_safe} ? JSON::PP::true : JSON::PP::false
        if defined $o{parallel_safe};
    $spec{planner} = \%planner if %planner;
    return $JSON->encode(\%spec);
}

# ---- fixtures (verbatim) --------------------------------------------------
our $SETUP_SQL = <<'ENDPPMSQL';

SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS pathprobe;

DROP SCHEMA IF EXISTS ppm CASCADE;
DROP SCHEMA IF EXISTS ppm_hidden CASCADE;
CREATE SCHEMA ppm;
CREATE SCHEMA ppm_hidden;

CREATE TABLE ppm.allowed (id int, v int);
INSERT INTO ppm.allowed SELECT g, g % 100 FROM generate_series(1, 2000) g;
CREATE TABLE ppm.private (id int, secret text);
INSERT INTO ppm.private SELECT g, 'x' FROM generate_series(1, 500) g;
CREATE TABLE ppm.cols (id int, pub int, secret text);
INSERT INTO ppm.cols SELECT g, g % 10, 'x' FROM generate_series(1, 500) g;
CREATE TABLE ppm.rls (id int, owner name, v int);
INSERT INTO ppm.rls SELECT g, 'ppm_agent_tbl', g FROM generate_series(1, 200) g;
ALTER TABLE ppm.rls ENABLE ROW LEVEL SECURITY;
CREATE POLICY ppm_rls_p ON ppm.rls USING (owner = current_user);
CREATE TABLE ppm.owned (id int, v int);
INSERT INTO ppm.owned SELECT g, g FROM generate_series(1, 300) g;
CREATE VIEW ppm.v_allowed AS SELECT * FROM ppm.allowed;
CREATE VIEW ppm.v_private AS SELECT * FROM ppm.private;
CREATE TABLE ppm_hidden.h (id int);
INSERT INTO ppm_hidden.h SELECT generate_series(1, 50);
ANALYZE ppm.allowed, ppm.private, ppm.cols, ppm.rls, ppm.owned, ppm_hidden.h;

-- roles
DROP ROLE IF EXISTS ppm_owner;
DROP ROLE IF EXISTS ppm_agent_tbl;
DROP ROLE IF EXISTS ppm_agent_col;
DROP ROLE IF EXISTS ppm_agent_none;
DROP ROLE IF EXISTS ppm_agent_noschema;
DROP ROLE IF EXISTS ppm_intruder;
CREATE ROLE ppm_owner LOGIN;
CREATE ROLE ppm_agent_tbl LOGIN;
CREATE ROLE ppm_agent_col LOGIN;
CREATE ROLE ppm_agent_none LOGIN;
CREATE ROLE ppm_agent_noschema LOGIN;
CREATE ROLE ppm_intruder LOGIN;

ALTER TABLE ppm.owned OWNER TO ppm_owner;

GRANT USAGE ON SCHEMA ppm TO ppm_owner, ppm_agent_tbl, ppm_agent_col, ppm_agent_none;
GRANT SELECT ON ppm.allowed TO ppm_agent_tbl;
GRANT SELECT (id, pub) ON ppm.cols TO ppm_agent_col;      -- not secret
GRANT SELECT ON ppm.rls TO ppm_agent_tbl;
GRANT SELECT ON ppm.v_allowed TO ppm_agent_tbl;
GRANT SELECT ON ppm.v_private TO ppm_agent_tbl;            -- view only, not base
-- ppm_agent_noschema: table SELECT but no schema USAGE on ppm_hidden
GRANT SELECT ON ppm_hidden.h TO ppm_agent_noschema;
-- ppm_owner can select its own table
-- ppm_agent_none / ppm_intruder get nothing beyond (maybe) schema USAGE

-- helper: run a pathprobe call, classify result without aborting the session.
CREATE OR REPLACE FUNCTION ppm_call(fn text, q text, spec text) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE res text; msg text; st text;
BEGIN
  IF    fn = 'propose_json' THEN res := pathprobe_propose_json(q, spec);
  ELSIF fn = 'propose'      THEN res := pathprobe_propose(q, spec);
  ELSIF fn = 'observe_json' THEN res := pathprobe_json(q);
  ELSE  RAISE EXCEPTION 'ppm_call: unknown fn %', fn;
  END IF;
  RETURN 'OK' || chr(30) || '' || chr(30) || res;
EXCEPTION WHEN OTHERS THEN
  GET STACKED DIAGNOSTICS st = RETURNED_SQLSTATE, msg = MESSAGE_TEXT;
  RETURN 'ERR' || chr(30) || st || chr(30) || msg;
END $$;
GRANT EXECUTE ON FUNCTION ppm_call(text, text, text) TO PUBLIC;
RESET client_min_messages;
ENDPPMSQL

our $TEARDOWN_SQL = <<'ENDPPMSQL';

SET client_min_messages = warning;
DROP FUNCTION IF EXISTS ppm_call(text, text, text);
DROP OWNED BY ppm_owner, ppm_agent_tbl, ppm_agent_col, ppm_agent_none,
              ppm_agent_noschema, ppm_intruder;
DROP SCHEMA IF EXISTS ppm CASCADE;
DROP SCHEMA IF EXISTS ppm_hidden CASCADE;
DROP ROLE IF EXISTS ppm_owner;
DROP ROLE IF EXISTS ppm_agent_tbl;
DROP ROLE IF EXISTS ppm_agent_col;
DROP ROLE IF EXISTS ppm_agent_none;
DROP ROLE IF EXISTS ppm_agent_noschema;
DROP ROLE IF EXISTS ppm_intruder;
RESET client_min_messages;
ENDPPMSQL

# ---- explicit smoke cases -------------------------------------------------

sub _sc {   # security case hashref
    my (%o) = @_;
    $o{fn}        //= 'propose_json';
    $o{expect}    //= {};
    $o{forbidden} //= [];
    $o{dims}      //= {};
    return \%o;
}
sub _cc {   # correctness case hashref
    my (%o) = @_;
    $o{fn}          //= 'propose_json';
    $o{parallelism} //= 'pinned_off';
    $o{require}     //= [];
    $o{dims}        //= {};
    return \%o;
}

sub mandatory_security_cases {
    return (
        _sc(cid=>'sec_allowed_ok', role=>'agent_table_select',
            query=>'SELECT * FROM ppm.allowed WHERE v>5', spec=>'{"advise":true}',
            expect=>{OK=>1},
            dims=>{relation=>'allowed_table',query_shape=>'top_select',spec_shape=>'advisor_true'}),
        _sc(cid=>'sec_private_top', role=>'agent_table_select',
            query=>'SELECT * FROM ppm.private', spec=>'{"advise":true}',
            expect=>{DENIED=>1,NO_LEAK=>1}, forbidden=>[],
            dims=>{relation=>'private_table',query_shape=>'top_select',spec_shape=>'advisor_true'}),
        _sc(cid=>'sec_private_cte', role=>'agent_table_select',
            query=>'WITH c AS (SELECT * FROM ppm.private) SELECT * FROM c', spec=>'{"advise":true}',
            expect=>{DENIED=>1,NO_LEAK=>1},
            dims=>{relation=>'private_table',query_shape=>'cte',spec_shape=>'advisor_true'}),
        _sc(cid=>'sec_private_subquery', role=>'agent_table_select',
            query=>'SELECT * FROM (SELECT * FROM ppm.private) s', spec=>'{"advise":true}',
            expect=>{DENIED=>1,NO_LEAK=>1},
            dims=>{relation=>'private_table',query_shape=>'subquery',spec_shape=>'advisor_true'}),
        _sc(cid=>'sec_private_sublink', role=>'agent_table_select',
            query=>'SELECT * FROM ppm.allowed a WHERE EXISTS (SELECT 1 FROM ppm.private p WHERE p.id=a.id)',
            spec=>'{"advise":true}', expect=>{DENIED=>1,NO_LEAK=>1},
            dims=>{relation=>'private_table',query_shape=>'exists_sublink',spec_shape=>'advisor_true'}),
        _sc(cid=>'sec_private_union', role=>'agent_table_select',
            query=>'SELECT id FROM ppm.allowed UNION SELECT id FROM ppm.private', spec=>'{"advise":true}',
            expect=>{DENIED=>1,NO_LEAK=>1},
            dims=>{relation=>'private_table',query_shape=>'union_branch',spec_shape=>'advisor_true'}),
        _sc(cid=>'sec_hypo_allowed_col', role=>'agent_column_select',
            query=>'SELECT id, pub FROM ppm.cols WHERE pub>5',
            spec=>'{"hypo_indexes":[{"relname":"cols","columns":["pub"]}]}', expect=>{OK=>1},
            dims=>{relation=>'column_restricted_table',query_shape=>'top_select',spec_shape=>'structured_hypo_allowed_column'}),
        _sc(cid=>'sec_hypo_forbidden_col', role=>'agent_column_select',
            query=>'SELECT id, pub FROM ppm.cols WHERE pub>5',
            spec=>'{"hypo_indexes":[{"relname":"cols","columns":["secret"]}]}',
            expect=>{DENIED=>1,NO_LEAK=>1}, forbidden=>[],
            dims=>{relation=>'column_restricted_table',query_shape=>'top_select',spec_shape=>'structured_hypo_forbidden_column'}),
        _sc(cid=>'sec_hypo_mixed_col', role=>'agent_column_select',
            query=>'SELECT id, pub FROM ppm.cols WHERE pub>5',
            spec=>'{"hypo_indexes":[{"relname":"cols","columns":["pub","secret"]}]}',
            expect=>{DENIED=>1,NO_LEAK=>1},
            dims=>{relation=>'column_restricted_table',query_shape=>'top_select',spec_shape=>'structured_hypo_mixed_columns'}),
        _sc(cid=>'sec_hypo_sql_nonsuper', role=>'agent_table_select',
            query=>'SELECT * FROM ppm.allowed',
            spec=>'{"hypo_sql":["CREATE INDEX zz ON ppm.allowed(id)"]}', expect=>{DENIED=>1},
            dims=>{relation=>'allowed_table',query_shape=>'top_select',spec_shape=>'hypo_sql'}),
        _sc(cid=>'sec_forbidden_malformed_hints', role=>'agent_table_select',
            query=>'SELECT * FROM ppm.private',
            spec=>'{"advise":true,"hints":"notanobject"}', expect=>{DENIED=>1},
            dims=>{relation=>'private_table',query_shape=>'top_select',spec_shape=>'hints_malformed'}),
        _sc(cid=>'sec_denied_hint_no_guc_leak', role=>'agent_table_select',
            query=>'SELECT * FROM ppm.private',
            spec=>'{"advise":true,"hints":{"forbid":["seqscan"]}}', expect=>{DENIED=>1,NO_GUC_LEAK=>1},
            dims=>{relation=>'private_table',query_shape=>'top_select',spec_shape=>'hints_valid'}),
        _sc(cid=>'sec_rls_nonsuper', role=>'agent_table_select',
            query=>'SELECT * FROM ppm.rls', spec=>'{"advise":true}', expect=>{DENIED=>1},
            dims=>{relation=>'rls_table',query_shape=>'top_select',spec_shape=>'advisor_true'}),
        _sc(cid=>'sec_view_over_private', role=>'agent_table_select',
            query=>'SELECT * FROM ppm.v_private', spec=>'{"advise":true}',
            expect=>{DENIED=>1,NO_LEAK=>1}, forbidden=>['ppm.private','private'],
            dims=>{relation=>'view_over_private_table',query_shape=>'top_select',spec_shape=>'advisor_true'}),
        _sc(cid=>'sec_superuser_full', role=>'superuser',
            query=>'SELECT * FROM ppm.private',
            spec=>'{"hypo_sql":["CREATE INDEX ww ON ppm.private(id)"]}', expect=>{OK=>1},
            dims=>{relation=>'private_table',query_shape=>'top_select',spec_shape=>'hypo_sql'}),
    );
}

sub mandatory_correctness_cases {
    return (
        _cc(cid=>'cor_target_missing',
            query=>'SELECT * FROM ppm.allowed WHERE v>5',
            spec=>_cand_spec(total_cost=>0.5, relname=>'nosuchtable'),
            allowed_fates=>{target_not_found=>1}, require=>['candidates','events'],
            dims=>{target=>'target_missing',policy=>'winner_forbidden_false'}),
        _cc(cid=>'cor_cheap_injected',
            query=>'SELECT * FROM ppm.allowed WHERE id=42',
            spec=>_cand_spec(total_cost=>0.5, parallel_safe=>0),
            allowed_fates=>{survived=>1,survived_upper=>1,displaced=>1},
            require=>['candidates','events'],
            dims=>{target=>'target_exists',candidate_cost=>'cheap'}),
        _cc(cid=>'cor_expensive_candidate',
            query=>'SELECT * FROM ppm.allowed WHERE id=42',
            spec=>_cand_spec(total_cost=>1e9, parallel_safe=>0),
            allowed_fates=>{rejected=>1,displaced=>1},
            require=>['candidates','events'],
            dims=>{target=>'target_exists',candidate_cost=>'expensive'}),
        _cc(cid=>'cor_winner_forbidden_pinned',
            query=>'SELECT * FROM ppm.allowed WHERE id=42',
            spec=>_cand_spec(total_cost=>0.5, winner_forbidden=>1),
            parallelism=>'pinned_off', allowed_fates=>{survived_upper=>1},
            require=>['candidates'],
            dims=>{policy=>'winner_forbidden_true',parallelism=>'pinned_off'}),
        _cc(cid=>'cor_pathkeys_claimed',
            query=>'SELECT * FROM ppm.allowed ORDER BY id',
            spec=>_cand_spec(total_cost=>0.5, pathkeys=>['id']),
            allowed_fates=>undef, require=>['candidates','events'],
            dims=>{pathkeys=>'claimed'}),
        _cc(cid=>'cor_advisor_block',
            query=>'SELECT * FROM ppm.allowed WHERE id=42 AND v=3',
            spec=>'{"advise":true}', allowed_fates=>undef, require=>['advisor'],
            advisor_expected=>1, dims=>{advisor=>'enabled'}),
        _cc(cid=>'cor_hypo_consistency',
            query=>'SELECT * FROM ppm.allowed WHERE id=42',
            spec=>'{"hypo_indexes":[{"relname":"allowed","columns":["id"]}]}',
            allowed_fates=>undef, require=>['hypo_indexes'],
            dims=>{hypo=>'structured_btree'}),
        _cc(cid=>'cor_truncation',
            query=>'SELECT * FROM ppm.allowed a JOIN ppm.allowed b ON a.id=b.id '
                  .'JOIN ppm.allowed c ON b.id=c.id JOIN ppm.allowed d ON c.id=d.id',
            spec=>'{"advise":true}', allowed_fates=>undef, require=>['limits'],
            max_events=>16, expect_truncated=>1, dims=>{limits=>'low_max_events'}),
        _cc(cid=>'cor_seq_monotonic',
            query=>'SELECT * FROM ppm.allowed WHERE id=42',
            spec=>_cand_spec(total_cost=>0.5), allowed_fates=>undef, require=>['events'],
            dims=>{invariant=>'seq_monotonic'}),
        _cc(cid=>'cor_label_origin',
            query=>'SELECT * FROM ppm.allowed WHERE id=42',
            spec=>_cand_spec(total_cost=>0.5, provider=>'model', pathname=>'mycand'),
            allowed_fates=>undef, require=>['candidates','events'],
            dims=>{invariant=>'label_origin'}),
    );
}

# ---- oracle ---------------------------------------------------------------

my %_ROLE_READABLE = (
    agent_table_select    => { allowed_table => 1 },
    agent_column_select   => { column_restricted_table => 1 },
    agent_no_select       => {},
    agent_no_schema_usage => {},
    intruder              => {},
    table_owner           => { owned_table => 1 },
);
my %_REL_COLUMNS = (
    allowed_table           => { id=>1, v=>1 },
    private_table           => { id=>1, secret=>1 },
    column_restricted_table => { id=>1, pub=>1, secret=>1 },
    rls_table               => { id=>1, owner=>1, v=>1 },
    view_over_allowed_table => { id=>1, v=>1 },
    view_over_private_table => { id=>1, secret=>1 },
);
my %_IS_VIEW = (view_over_allowed_table=>1, view_over_private_table=>1);
my %_ALWAYS_FORBIDDEN = (
    private_table=>1, rls_table=>1,
    view_over_allowed_table=>1, view_over_private_table=>1,
);

sub _spec_valid_on {
    my ($relation, $spec_shape) = @_;
    return 0 if $spec_shape eq 'none';
    if (index($spec_shape, 'structured_hypo') == 0) {
        return 0 if $_IS_VIEW{$relation};
        my $cols = $_REL_COLUMNS{$relation};
        return ($cols->{id} ? 1 : 0) if $spec_shape eq 'structured_hypo_allowed_column';
        return ($cols->{secret} ? 1 : 0);
    }
    return 1;
}

sub _access_denied {
    my ($role, $relation, $qshape, $sshape) = @_;
    return 0 if $role eq 'superuser';
    return 1 if $qshape eq 'multi_statement' || $qshape eq 'dml_or_modifying_cte';
    return 1 if $sshape eq 'hypo_sql';
    return 1 if $_IS_VIEW{$relation} || $relation eq 'rls_table';
    return 1 if $relation eq 'private_table';
    return 1 if $sshape eq 'structured_hypo_forbidden_column'
             || $sshape eq 'structured_hypo_mixed_columns';
    return ($_ROLE_READABLE{$role}{$relation} ? 0 : 1);
}

# returns hashref of expectations or undef (exclude)
sub security_oracle {
    my ($role, $relation, $qshape, $sshape) = @_;
    return undef unless _spec_valid_on($relation, $sshape);
    my $denied = _access_denied($role, $relation, $qshape, $sshape);
    if ($sshape eq 'hints_malformed') {
        return $denied ? {DENIED=>1} : undef;
    }
    if ($denied) {
        my %exp = (DENIED=>1);
        $exp{NO_LEAK} = 1
            if $relation eq 'view_over_private_table'
            || $sshape eq 'structured_hypo_forbidden_column'
            || $sshape eq 'structured_hypo_mixed_columns';
        return \%exp;
    }
    return {OK=>1};
}

# ---- query / spec builders for generated cases ----------------------------

my %_REL_SQLNAME = (
    allowed_table=>'ppm.allowed', private_table=>'ppm.private',
    column_restricted_table=>'ppm.cols', rls_table=>'ppm.rls',
    view_over_allowed_table=>'ppm.v_allowed', view_over_private_table=>'ppm.v_private',
);
my %_BARE = (
    allowed_table=>'allowed', private_table=>'private',
    column_restricted_table=>'cols', rls_table=>'rls',
    view_over_allowed_table=>'v_allowed', view_over_private_table=>'v_private',
);

sub _query_for {
    my ($relation, $qshape) = @_;
    my $rel = $_REL_SQLNAME{$relation};
    my $cols = $relation eq 'column_restricted_table' ? 'id, pub' : '*';
    return "SELECT $cols FROM $rel"                                  if $qshape eq 'top_select';
    return "WITH c AS (SELECT $cols FROM $rel) SELECT * FROM c"      if $qshape eq 'cte';
    return "SELECT * FROM (SELECT $cols FROM $rel) s"                if $qshape eq 'subquery';
    return "SELECT * FROM ppm.allowed a WHERE EXISTS (SELECT 1 FROM $rel r WHERE r.id=a.id)"
                                                                    if $qshape eq 'exists_sublink';
    return "SELECT id FROM ppm.allowed UNION SELECT id FROM $rel"    if $qshape eq 'union_branch';
    return "SELECT a.* FROM ppm.allowed a JOIN $rel r ON a.id=r.id"  if $qshape eq 'join';
    return "SELECT $cols FROM $rel; SELECT 1"                        if $qshape eq 'multi_statement';
    return "UPDATE $rel SET id=id WHERE false"                       if $qshape eq 'dml_or_modifying_cte';
    return undef;
}

sub _spec_for {
    my ($relation, $sshape) = @_;
    my $rel = $_BARE{$relation};
    return '{}'                                    if $sshape eq 'none';
    return '{"advise":true}'                       if $sshape eq 'advisor_true';
    return '{"advise":true,"hints":{"forbid":["seqscan"]}}' if $sshape eq 'hints_valid';
    return '{"advise":true,"hints":"notanobject"}' if $sshape eq 'hints_malformed';
    return '{"hypo_sql":["CREATE INDEX zz ON ppm.allowed(id)"]}' if $sshape eq 'hypo_sql';
    if ($sshape eq 'structured_hypo_allowed_column') {
        my $col = $relation eq 'column_restricted_table' ? 'pub' : 'id';
        return qq({"hypo_indexes":[{"relname":"$rel","columns":["$col"]}]});
    }
    if ($sshape eq 'structured_hypo_forbidden_column') {
        return qq({"hypo_indexes":[{"relname":"$rel","columns":["secret"]}]});
    }
    if ($sshape eq 'structured_hypo_mixed_columns') {
        my $col = $relation eq 'column_restricted_table' ? 'pub' : 'id';
        return qq({"hypo_indexes":[{"relname":"$rel","columns":["$col","secret"]}]});
    }
    return _cand_spec(total_cost=>0.5, relname=>'allowed') if $sshape eq 'candidate_allowed_rel';
    return _cand_spec(total_cost=>0.5, relname=>'private') if $sshape eq 'candidate_private_rel';
    return undef;
}

# ---- constraints (with reasons) ------------------------------------------

sub _constraint_skip {
    my ($role, $relation, $qshape, $sshape) = @_;
    unless (_spec_valid_on($relation, $sshape)) {
        return "spec 'none' is not a probe call (target.relname required)" if $sshape eq 'none';
        return "spec_shape structurally invalid on relation (hypo on view / no such column)";
    }
    return "superuser bypass path is uniform (representatives kept in smoke/targeted)"
        if $role eq 'superuser';
    return "multi_statement rejected at raw-parse guard (one representative kept)"
        if $qshape eq 'multi_statement' && $sshape ne 'advisor_true';
    return "modifying command rejected before spec (one representative kept)"
        if $qshape eq 'dml_or_modifying_cte' && $sshape ne 'advisor_true';
    return "RLS fail-closed precedes hypo column check"
        if $relation eq 'rls_table' && index($sshape,'structured_hypo')==0;
    return "view fail-closed precedes candidate spec"
        if $_IS_VIEW{$relation}
        && ($sshape eq 'candidate_allowed_rel' || $sshape eq 'candidate_private_rel');
    return "hints_malformed covered by top_select allowed/forbidden representatives"
        if $sshape eq 'hints_malformed' && $qshape ne 'top_select';
    return undef;
}

sub _mk_security_case {
    my ($role, $relation, $qshape, $sshape, $prefix) = @_;
    my $exp = security_oracle($role, $relation, $qshape, $sshape);
    return (undef, "undecided by conservative oracle") unless $exp;
    my $q = _query_for($relation, $qshape);
    my $s = _spec_for($relation, $sshape);
    return (undef, "no query/spec builder") unless defined $q && defined $s;
    my @forb;
    if    ($relation eq 'view_over_private_table') { @forb = ('ppm.private','private'); }
    elsif ($sshape eq 'structured_hypo_forbidden_column'
        || $sshape eq 'structured_hypo_mixed_columns') { @forb = ('secret'); }
    my $cid = "${prefix}_${role}__${relation}__${qshape}__${sshape}";
    return (_sc(cid=>$cid, role=>$role, query=>$q, spec=>$s, expect=>$exp,
                forbidden=>\@forb,
                dims=>{role=>$role,relation=>$relation,query_shape=>$qshape,spec_shape=>$sshape}),
            undef);
}

# ---- greedy all-pairs (faithful port for count parity) --------------------

sub _product {
    my @dims = @_;
    my @out = ([]);
    for my $d (@dims) {
        my @next;
        for my $pref (@out) { push @next, [@$pref, $_] for @$d; }
        @out = @next;
    }
    return @out;
}

sub _all_pairs {
    my @dims = @_;
    my $n = @dims;
    my @pairs;                       # ordered (i,j) i<j
    for (my $i=0; $i<$n; $i++) { for (my $j=$i+1; $j<$n; $j++) { push @pairs, [$i,$j]; } }
    my %need;
    for my $p (@pairs) {
        my ($i,$j) = @$p;
        for my $vi (@{$dims[$i]}) { for my $vj (@{$dims[$j]}) {
            $need{"$i\x00$vi\x00$j\x00$vj"} = 1;
        } }
    }
    my @candidates = _product(@dims);
    my %covered;
    my @chosen;
    while (1) {
        my $remaining = 0;
        for my $k (keys %need) { if (!$covered{$k}) { $remaining=1; last; } }
        last unless $remaining;
        my ($best, $best_gain) = (undef, -1);
        for my $tup (@candidates) {
            my $gain = 0;
            for my $p (@pairs) {
                my ($i,$j) = @$p;
                my $key = "$i\x00$tup->[$i]\x00$j\x00$tup->[$j]";
                $gain++ if $need{$key} && !$covered{$key};
            }
            if ($gain > $best_gain) { $best = $tup; $best_gain = $gain; }
        }
        last if !defined $best || $best_gain <= 0;
        push @chosen, $best;
        for my $p (@pairs) {
            my ($i,$j) = @$p;
            $covered{"$i\x00$best->[$i]\x00$j\x00$best->[$j]"} = 1;
        }
    }
    return @chosen;
}

# ---- security generation --------------------------------------------------

sub _pairwise_security {
    my @combos = _all_pairs(\@ROLES, \@RELATIONS, \@QUERY_SHAPES, \@SPEC_SHAPES);
    my (@cases, @skips, %seen);
    for my $t (@combos) {
        my ($role,$relation,$qshape,$sshape) = @$t;
        my %dims = (role=>$role,relation=>$relation,query_shape=>$qshape,spec_shape=>$sshape);
        my $reason = _constraint_skip($role,$relation,$qshape,$sshape);
        if ($reason) { push @skips, [\%dims,$reason]; next; }
        my ($case,$why) = _mk_security_case($role,$relation,$qshape,$sshape,'pw');
        if (!$case) { push @skips, [\%dims,$why]; next; }
        next if $seen{$case->{cid}}++;
        push @cases, $case;
    }
    return (\@cases, \@skips);
}

my %_TARGET_GROUPS = (
    role_query_relpriv => {
        roles=>['agent_table_select','agent_column_select','intruder'],
        relations=>['allowed_table','private_table'],
        qshapes=>['top_select','cte','subquery','exists_sublink','union_branch','join'],
        sshapes=>['advisor_true'] },
    role_allowed_hints => {
        roles=>['agent_table_select','superuser'],
        relations=>['allowed_table','private_table'],
        qshapes=>['top_select'], sshapes=>['hints_malformed'] },
    role_hypo_colpriv => {
        roles=>['agent_column_select','superuser'],
        relations=>['column_restricted_table'], qshapes=>['top_select'],
        sshapes=>['structured_hypo_allowed_column','structured_hypo_forbidden_column','structured_hypo_mixed_columns'] },
    advisor_hypo_colpriv => {
        roles=>['agent_column_select','superuser'],
        relations=>['column_restricted_table'], qshapes=>['top_select'],
        sshapes=>['advisor_true'] },
    view_checkasuser => {
        roles=>['agent_table_select','agent_column_select'],
        relations=>['view_over_allowed_table','view_over_private_table'],
        qshapes=>['top_select'], sshapes=>['advisor_true'] },
);
# deterministic group order
my @_TARGET_ORDER = qw(role_query_relpriv role_allowed_hints role_hypo_colpriv
                       advisor_hypo_colpriv view_checkasuser);

sub _targeted_security {
    my (@cases, @skips, %seen);
    for my $gname (@_TARGET_ORDER) {
        my $g = $_TARGET_GROUPS{$gname};
        for my $t (_product($g->{roles},$g->{relations},$g->{qshapes},$g->{sshapes})) {
            my ($role,$relation,$qshape,$sshape) = @$t;
            my %dims = (role=>$role,relation=>$relation,query_shape=>$qshape,spec_shape=>$sshape,group=>$gname);
            my $reason = _constraint_skip($role,$relation,$qshape,$sshape);
            if ($reason) { push @skips, [\%dims,$reason]; next; }
            my ($case,$why) = _mk_security_case($role,$relation,$qshape,$sshape,"t_$gname");
            if (!$case) { push @skips, [\%dims,$why]; next; }
            next if $seen{$case->{cid}}++;
            if ($gname eq 'advisor_hypo_colpriv' && $case->{expect}{OK}) {
                $case->{expect}{NO_LEAK} = 1;
                $case->{forbidden} = ['secret'];
            }
            $case->{dims}{group} = $gname;
            push @cases, $case;
        }
    }
    return (\@cases, \@skips);
}

# generate_security($mode) -> (\@cases, \@skips)
# Modes accepted by the TAP tests; fuzz is a seeded runtime sample drawn by
# fuzz_sample(), not a deterministic case set.
our @MODES = qw(smoke pairwise targeted fuzz);

sub check_mode {
    my ($mode) = @_;
    die "unknown model mode '$mode' (expected: @MODES)\n"
        unless grep { $_ eq $mode } @MODES;
}

sub _deterministic_mode {
    my ($mode) = @_;
    die "mode '$mode' has no deterministic case set (expected: smoke pairwise targeted)\n"
        unless grep { $_ eq $mode } qw(smoke pairwise targeted);
}

sub generate_security {
    my ($mode) = @_;
    _deterministic_mode($mode);
    return ([ mandatory_security_cases() ], []) if $mode eq 'smoke';
    return _targeted_security() if $mode eq 'targeted';
    return _pairwise_security();
}

# ---- correctness generation ----------------------------------------------

sub generate_correctness {
    my ($mode) = @_;
    _deterministic_mode($mode);
    return [ mandatory_correctness_cases() ] if $mode eq 'smoke';
    my @out = mandatory_correctness_cases();
    for my $par (@PARALLELISM) {
        for my $pol (@POLICIES) {
            for my $comp (qw(cheap expensive)) {
                my $wf = ($pol eq 'winner_forbidden_true') ? 1 : 0;
                my $cost = $comp eq 'cheap' ? 0.5 : 1e9;
                my %args = (total_cost=>$cost, winner_forbidden=>$wf);
                $args{parallel_safe} = 0 if $par eq 'pinned_off' && !$wf;
                my $spec = _cand_spec(%args);
                my %fates;
                if    ($wf && $par eq 'pinned_off') { %fates = (survived_upper=>1); }
                elsif ($wf)                         { %fates = (survived_upper=>1,rejected=>1); }
                elsif ($par eq 'pinned_off' && $comp eq 'cheap')     { %fates = (survived=>1,survived_upper=>1,displaced=>1); }
                elsif ($par eq 'pinned_off' && $comp eq 'expensive') { %fates = (rejected=>1,displaced=>1); }
                else { %fates = (survived=>1,survived_upper=>1,displaced=>1,rejected=>1); }
                push @out, _cc(cid=>"gen_par_${par}__${pol}__${comp}",
                    query=>'SELECT * FROM ppm.allowed WHERE id=42', spec=>$spec,
                    parallelism=>$par, allowed_fates=>\%fates, require=>['candidates'],
                    dims=>{parallelism=>$par,policy=>$pol,competition=>$comp});
            }
        }
    }
    return \@out;
}

# fuzz: seeded sample of the pairwise space
sub fuzz_sample {
    my ($seed) = @_;
    my ($sec,$skips) = _pairwise_security();
    my $cor = generate_correctness('pairwise');
    _shuffle($sec, $seed);
    _shuffle($cor, $seed);
    my $ns = int(@$sec/3); $ns = 1 if $ns < 1;
    my $nc = int(@$cor/2); $nc = 1 if $nc < 1;
    return ([ @$sec[0..$ns-1] ], [ @$cor[0..$nc-1] ], $skips);
}
sub _shuffle {   # deterministic LCG shuffle by seed
    my ($aref, $seed) = @_;
    my $s = ($seed || 0) & 0x7fffffff; $s ||= 1;
    for (my $i = @$aref-1; $i > 0; $i--) {
        $s = (1103515245*$s + 12345) & 0x7fffffff;
        my $j = $s % ($i+1);
        @$aref[$i,$j] = @$aref[$j,$i];
    }
}

# ---- runtime: build SQL, run in one session, parse ------------------------

my @GUC_WATCH = qw(enable_seqscan enable_indexscan enable_bitmapscan
                   enable_indexonlyscan enable_sort enable_hashjoin
                   enable_nestloop enable_mergejoin);
my %DENIED_SQLSTATE = ('42501'=>1, '0A000'=>1);
my $US = "\x1f"; my $RS = "\x1e";

sub _dollar {
    my ($tag, $s) = @_;
    die "delimiter \$$tag\$ collides" if index($s, "\$$tag\$") >= 0;
    return "\$$tag\$$s\$$tag\$";
}
sub _call_row {
    my ($fn,$q,$s) = @_;
    return "SELECT 'RES'||chr(31)||translate(encode(convert_to(ppm_call("
        ._dollar('FN',$fn).", "._dollar('Q',$q).", "._dollar('S',$s)
        ."),'UTF8'),'base64'), E'\\n','')";
}
sub _guc_row {
    my ($when) = @_;
    my $names = join(',', map {"'$_'"} @GUC_WATCH);
    return "SELECT 'GUC'||chr(31)||'$when'||chr(31)||"
        ."coalesce(string_agg(name||'='||setting, ',' ORDER BY name),'') "
        ."FROM pg_settings WHERE name IN ($names);";
}

sub build_security_sql {
    my ($c) = @_;
    my $role = $ROLE_SQL{$c->{role}};
    my @p;
    push @p, "SET ROLE $role;" if defined $role;
    push @p, "SET search_path = ppm, public;";
    my $guc = $c->{expect}{NO_GUC_LEAK};
    push @p, _guc_row('BEFORE') if $guc;
    push @p, _call_row($c->{fn}, $c->{query}, $c->{spec}).";";
    push @p, _guc_row('AFTER') if $guc;
    push @p, "RESET ROLE;" if defined $role;
    return join("\n", @p)."\n";
}
sub build_correctness_sql {
    my ($c) = @_;
    my @p = ("RESET ROLE;", "SET search_path = ppm, public;");
    if    ($c->{parallelism} eq 'pinned_off') { push @p, "SET max_parallel_workers_per_gather = 0;"; }
    elsif ($c->{parallelism} eq 'forced_on')  {
        push @p, "SET max_parallel_workers_per_gather = 4;",
                 "SET min_parallel_table_scan_size = 0;",
                 "SET parallel_setup_cost = 0;", "SET parallel_tuple_cost = 0;";
    }
    push @p, "SET pathprobe.max_events = $c->{max_events};" if defined $c->{max_events};
    push @p, _call_row($c->{fn}, $c->{query}, $c->{spec}).";";
    push @p, "RESET ALL;";
    return join("\n", @p)."\n";
}

# parse psql stdout -> (status, sqlstate, message, payload, {BEFORE=>..,AFTER=>..})
sub parse_output {
    my ($raw) = @_;
    my ($status,$sqlstate,$message,$payload);
    my %gucs;
    for my $line (split /\n/, $raw) {
        if (index($line, "RES$US") == 0) {
            my $b64 = substr($line, length("RES$US"));
            require MIME::Base64;
            my $dec = eval { MIME::Base64::decode_base64($b64) } // '';
            my @f = split /$RS/, $dec, -1;
            $status = $f[0];
            if (defined $status && $status eq 'OK') { $sqlstate=''; $payload = $f[2] // ''; }
            else { $sqlstate = $f[1] // ''; $message = $f[2] // ''; }
        }
        elsif (index($line, "GUC$US") == 0) {
            my $rest = substr($line, length("GUC$US"));
            my ($when,$vals) = split /$US/, $rest, 2;
            $gucs{$when} = $vals // '';
        }
    }
    return ($status,$sqlstate,$message,$payload,\%gucs);
}

sub _looks_json { my ($p)=@_; return 0 unless defined $p && length $p; my $ok = eval { $JSON->decode($p); 1 }; return $ok?1:0; }

sub _token_present {
    my ($name,$text) = @_;
    return 0 unless defined $text && length $text;
    my $re = qr/(?<![\w.])\Q$name\E(?![\w])/;
    return $text =~ $re ? 1 : 0;
}

sub check_security {
    my ($c,$status,$sqlstate,$message,$payload,$gucs) = @_;
    my @fails;
    my $text = ($message // '').($payload // '');
    if ($c->{expect}{OK}) {
        if (!defined $status || $status ne 'OK') {
            push @fails, "expected OK, got ".($status//'?')."/$sqlstate: ".substr($message//'',0,120);
        } elsif (!_looks_json($payload)) { push @fails, "OK but payload is not valid JSON"; }
    }
    if ($c->{expect}{DENIED}) {
        if (defined $status && $status eq 'OK') { push @fails, "expected DENIED, got OK"; }
        elsif (!$DENIED_SQLSTATE{$sqlstate//''}) {
            push @fails, "expected DENIED (permission error), got sqlstate ".($sqlstate//'')." : ".substr($message//'',0,120);
        }
    }
    if ($c->{expect}{NO_LEAK}) {
        my $supplied = ($c->{query}//'').' '.($c->{spec}//'');
        for my $name (@{$c->{forbidden}}) {
            next unless length $name;
            next if _token_present($name,$supplied);
            push @fails, "NO_LEAK violated: forbidden name '$name' in output/error"
                if _token_present($name,$text);
        }
    }
    if ($c->{expect}{NO_GUC_LEAK}) {
        my ($b,$a) = ($gucs->{BEFORE},$gucs->{AFTER});
        if (!defined $b || !defined $a) { push @fails, "NO_GUC_LEAK: missing GUC snapshot"; }
        elsif ($b ne $a) { push @fails, "NO_GUC_LEAK violated: $b != $a"; }
    }
    return \@fails;
}

sub check_correctness {
    my ($c,$status,$sqlstate,$message,$payload) = @_;
    my @fails;
    if (!defined $status || $status ne 'OK') {
        push @fails, "call failed (".($sqlstate//'')."): ".substr($message//'',0,160);
        return \@fails;
    }
    my $j = eval { $JSON->decode($payload) };
    if (!$j) { push @fails, "payload not JSON"; return \@fails; }
    for my $f (@{$c->{require}}) { push @fails, "missing required field '$f'" unless exists $j->{$f}; }
    my @ev = @{ $j->{events} // [] };
    my @seq = map { $_->{seq} } grep { ref $_ eq 'HASH' && exists $_->{seq} } @ev;
    for (my $i=0; $i<$#seq; $i++) { if ($seq[$i] >= $seq[$i+1]) { push @fails, "events[].seq not strictly increasing"; last; } }
    if (defined $c->{expect_truncated}) {
        my $lim = $j->{limits} // {};
        my $tr = $lim->{events_truncated} ? 1 : 0;
        push @fails, "events_truncated=$tr, expected $c->{expect_truncated}" if $tr != $c->{expect_truncated};
        my $cap = $lim->{max_events};
        push @fails, "emitted ".scalar(@ev)." events exceeds cap $cap" if defined $cap && @ev > $cap;
    }
    push @fails, "missing coverage object" unless ref($j->{coverage}) eq 'HASH';
    for my $cand (@{ $j->{candidates} // [] }) {
        next unless ref $cand eq 'HASH';
        my $ec = $cand->{event_counts} // {};
        my ($acc,$sur,$dis) = ($ec->{accepted}//0, $ec->{survived}//0, $ec->{displaced}//0);
        # counts only bound each other where acceptance is observed
        if (($j->{coverage} // {})->{add_path_accepted}) {
            push @fails, "candidate survived($sur) > accepted($acc)" if $sur > $acc;
            push @fails, "candidate displaced($dis) > accepted($acc)" if $dis > $acc;
        }
        my $f = $cand->{fate} // '';
        if ($f eq 'target_not_found' || $f eq 'not_examined') {
            push @fails, "$f but nonzero event_counts"
                if $acc || ($ec->{rejected}//0) || $sur || $dis;
        }
        push @fails, "fate '".($cand->{fate}//'')."' outside alphabet"
            unless $FATE_ALPHABET{$cand->{fate}//''};
    }
    if (defined $c->{allowed_fates}) {
        my @cands = @{ $j->{candidates} // [] };
        if (!@cands) { push @fails, "expected a candidate with a fate, none present"; }
        else {
            my $fate = $cands[0]{fate} // '';
            push @fails, "fate '$fate' not in allowed_fates ".join(',',sort keys %{$c->{allowed_fates}})
                unless $c->{allowed_fates}{$fate};
        }
    }
    for my $h (@{ $j->{hypo_indexes} // [] }) {
        next unless ref $h eq 'HASH';
        push @fails, "hypo in_plan but not injected" if $h->{in_plan} && !$h->{injected};
        push @fails, "hypo survived > accepted" if ($h->{survived}//0) > ($h->{accepted}//0);
    }
    if ($c->{advisor_expected}) {
        my $adv = $j->{advisor} // {};
        if (!$adv->{enabled}) { push @fails, "advisor expected enabled"; }
        else {
            push @fails, "advisor.enabled but no baseline_cost" unless exists $adv->{baseline_cost};
            push @fails, "advisor.enabled but no plan_cost" unless exists $adv->{plan_cost};
            if (exists $adv->{baseline_cost} && exists $adv->{plan_cost} && exists $adv->{query_level_benefit}) {
                push @fails, "query_level_benefit != baseline-plan"
                    if abs(($adv->{baseline_cost}-$adv->{plan_cost}) - $adv->{query_level_benefit}) > 1e-6;
            }
        }
    }
    return \@fails;
}

# run one case against a node; returns (ok, \@fails)
sub run_security_case {
    my ($node,$c) = @_;
    my $out='';
    $node->psql('postgres', build_security_sql($c),
                stdout=>\$out, on_error_stop=>0,
                extra_params=>['-A','-t','--no-psqlrc','-P','null=']);
    my @r = parse_output($out);
    my $fails = check_security($c, @r);
    return (scalar(@$fails)==0, $fails);
}
sub run_correctness_case {
    my ($node,$c) = @_;
    my $out='';
    $node->psql('postgres', build_correctness_sql($c),
                stdout=>\$out, on_error_stop=>0,
                extra_params=>['-A','-t','--no-psqlrc','-P','null=']);
    my ($status,$sqlstate,$message,$payload) = parse_output($out);
    my $fails = check_correctness($c,$status,$sqlstate,$message,$payload);
    return (scalar(@$fails)==0, $fails);
}

# coverage matrix text (for docs regeneration / --emit-matrix parity)
sub skip_summary {
    my ($skips) = @_;
    my %c;
    $c{$_->[1]}++ for @$skips;
    return \%c;
}

1;
