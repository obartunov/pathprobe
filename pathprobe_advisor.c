/*
 * pathprobe_advisor.c
 *		Local single-query index-candidate generation for pathprobe's
 *		"advise":true mode.  From one already-parsed query, derive btree
 *		index candidates from its restriction quals and pathkeys and feed
 *		them into the propose layer's hypothetical-index competition.
 *
 *		Deliberately a single-query heuristic: no workload history, no
 *		cross-query ranking, no maintenance/storage cost model.  This is
 *		not a production index advisor.
 */
#include "postgres.h"

#include <math.h>

#include "access/amapi.h"
#include "access/itup.h"
#include "catalog/pg_am_d.h"
#include "catalog/namespace.h"
#include "miscadmin.h"
#include "utils/acl.h"
#include "utils/guc.h"
#include "optimizer/restrictinfo.h"
#include "parser/parsetree.h"
#include "utils/ruleutils.h"
#include "parser/parse_utilcmd.h"
#include "commands/defrem.h"
#include "utils/index_selfuncs.h"

#include "commands/explain.h"
#include "executor/executor.h"
#include "fmgr.h"
#include "lib/stringinfo.h"
#include "nodes/bitmapset.h"
#include "nodes/extensible.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "nodes/pathnodes.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "parser/parser.h"
#include "utils/builtins.h"
#include "utils/json.h"
#include "utils/jsonfuncs.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "tcop/tcopprot.h"		/* pg_parse_query, pg_plan_queries */
#include "tcop/utility.h"

#include "pathprobe.h"
#include "pathprobe_security.h"
#include "pathprobe_propose.h"

/*
 * Advisor (increment 1, btree): derive index candidates for this rel from
 * rel->baserestrictinfo and root->query_pathkeys, then feed them into the
 * ordinary hypothetical-index competition.  Capability test is catalog
 * driven: an operator qualifies iff it has a strategy in the column's
 * default btree opclass family (the same opclass a real CREATE INDEX would
 * use).  Candidates: single-column per qualifying qual (equalities first),
 * one composite (equalities + first range column), one ASC ordered set from
 * query_pathkeys.  Hard cap below keeps the run to one planning pass.
 */
#define PATHPROBE_ADVISE_MAX 6

/*
 * Index atom: a unique indexable argument collected from the query.
 * attno > 0 for plain columns; attno == 0 for expression atoms (expr set).
 * Extraction sources:
 *   - EquivalenceClasses with ec_has_const: the planner already deduped
 *     equality arguments, stripped RelabelType and applied transitivity
 *     (a = b AND b = 5 yields the atom a as well);
 *   - baserestrictinfo: side selection by the precomputed
 *     rinfo->left_relids/right_relids (the rule match_opclause_to_indexcol
 *     itself uses), plus degenerate SAOP/NullTest branches which have no
 *     left/right relids.
 */
typedef struct AdvCol
{
	AttrNumber	attno;			/* 0 = expression atom */
	Expr	   *expr;			/* expression when attno == 0 */
	bool		is_eq;
}			AdvCol;

/* catalog capability test: type has a default btree opclass; op (if any)
 * has a strategy in its family */
static int
adv_btree_strategy(Oid opno, Oid atttype)
{
	Oid			opclass = GetDefaultOpClass(atttype, BTREE_AM_OID);

	if (!OidIsValid(opclass))
		return 0;
	if (!OidIsValid(opno))
		return BTEqualStrategyNumber;	/* IS NULL / EC member */
	return get_op_opfamily_strategy(opno, get_opclass_family(opclass));
}

static bool
adv_atom_from_node(RelOptInfo *rel, Node *node, Oid opno, AdvCol * out)
{
	if (node == NULL)
		return false;
	if (IsA(node, RelabelType))
		node = (Node *) ((RelabelType *) node)->arg;
	if (contain_volatile_functions(node))
		return false;
	if (IsA(node, Var))
	{
		Var		   *v = (Var *) node;

		if (v->varno != rel->relid || v->varattno <= 0)
			return false;
		out->attno = v->varattno;
		out->expr = NULL;
	}
	else
	{
		if (IsA(node, Const) || IsA(node, Param))
			return false;
		out->attno = 0;
		out->expr = (Expr *) node;
	}
	{
		int			strat = adv_btree_strategy(opno, exprType(node));

		if (strat <= 0)
			return false;
		out->is_eq = (strat == BTEqualStrategyNumber);
	}
	return true;
}

static bool
adv_col_btree_qual(RelOptInfo *rel, RangeTblEntry *rte, RestrictInfo *rinfo,
				   AdvCol * out)
{
	Expr	   *clause = rinfo->clause;

	/* IS NULL: indexable via btree amsearchnulls; equality-like */
	if (IsA(clause, NullTest))
	{
		NullTest   *nt = (NullTest *) clause;

		if (nt->nulltesttype != IS_NULL || nt->argisrow)
			return false;		/* IS NOT NULL skipped: rarely selective */
		return adv_atom_from_node(rel, (Node *) nt->arg, InvalidOid, out);
	}

	/* col = ANY(array): btree native via amsearcharray.  SAOP has no
	 * precomputed left/right relids, hence the explicit array-side check. */
	if (IsA(clause, ScalarArrayOpExpr))
	{
		ScalarArrayOpExpr *saop = (ScalarArrayOpExpr *) clause;

		if (!saop->useOr || list_length(saop->args) != 2)
			return false;
		if (contain_var_clause((Node *) lsecond(saop->args)))
			return false;
		return adv_atom_from_node(rel, (Node *) linitial(saop->args),
								  saop->opno, out);
	}

	/* binary operator: pick the side by precomputed relids, the same rule
	 * match_opclause_to_indexcol applies (indxpath.c) */
	if (IsA(clause, OpExpr))
	{
		OpExpr	   *op = (OpExpr *) clause;

		if (list_length(op->args) != 2)
			return false;
		if (bms_is_subset(rinfo->left_relids, rel->relids) &&
			bms_is_empty(rinfo->right_relids))
			return adv_atom_from_node(rel, (Node *) linitial(op->args),
									  op->opno, out);
		if (bms_is_subset(rinfo->right_relids, rel->relids) &&
			bms_is_empty(rinfo->left_relids))
		{
			Oid			commut = get_commutator(op->opno);

			/*
			 * A non-commutable operator (LIKE etc.) cannot become an
			 * index qual with the Var on the right; InvalidOid must not
			 * leak into the strategy test, which treats it as the
			 * equality-like marker for IS NULL / EC / boolean callers.
			 */
			if (!OidIsValid(commut))
				return false;
			return adv_atom_from_node(rel, (Node *) lsecond(op->args),
									  commut, out);
		}
		return false;
	}

	/*
	 * Boolean quals, mirroring match_boolean_index_clause (indxpath.c):
	 * bare boolean clause (col / f(x)), NOT arg, BooleanTest IS TRUE /
	 * IS FALSE.  All are equality-like (col = true/false) for a btree
	 * candidate.  Other BooleanTest forms are not indexed by the core
	 * either.
	 */
	if (IsA(clause, BoolExpr) &&
		((BoolExpr *) clause)->boolop == NOT_EXPR &&
		list_length(((BoolExpr *) clause)->args) == 1)
	{
		AdvCol		c;

		if (adv_atom_from_node(rel,
							   (Node *) linitial(((BoolExpr *) clause)->args),
							   InvalidOid, &c) &&
			exprType((Node *) linitial(((BoolExpr *) clause)->args)) == BOOLOID)
		{
			*out = c;
			out->is_eq = true;
			return true;
		}
		return false;
	}
	if (IsA(clause, BooleanTest))
	{
		BooleanTest *bt = (BooleanTest *) clause;

		if (bt->booltesttype != IS_TRUE && bt->booltesttype != IS_FALSE)
			return false;
		if (adv_atom_from_node(rel, (Node *) bt->arg, InvalidOid, out))
		{
			out->is_eq = true;
			return true;
		}
		return false;
	}
	if (exprType((Node *) clause) == BOOLOID &&
		adv_atom_from_node(rel, (Node *) clause, InvalidOid, out))
	{
		out->is_eq = true;
		return true;
	}
	return false;
}

/*
 * adv_atoms_from_ecs - equality atoms from the planner's own dedup:
 * every non-const member of a const-bearing EquivalenceClass that belongs
 * entirely to this rel.  Transitive equalities come for free.
 */
static int
adv_atoms_from_ecs(PlannerInfo *root, RelOptInfo *rel,
				   AdvCol * cols, int ncols, int maxcols)
{
	ListCell   *lc;

	foreach(lc, root->eq_classes)
	{
		EquivalenceClass *ec = (EquivalenceClass *) lfirst(lc);
		ListCell   *lm;

		if (!ec->ec_has_const || !bms_overlap(ec->ec_relids, rel->relids))
			continue;
		foreach(lm, ec->ec_members)
		{
			EquivalenceMember *em = (EquivalenceMember *) lfirst(lm);
			AdvCol		c;
			int			i;

			if (em->em_is_const ||
				!bms_is_subset(em->em_relids, rel->relids) ||
				bms_is_empty(em->em_relids))
				continue;
			if (!adv_atom_from_node(rel, (Node *) em->em_expr,
									InvalidOid, &c))
				continue;
			c.is_eq = true;
			for (i = 0; i < ncols; i++)
				if ((c.attno > 0 && cols[i].attno == c.attno) ||
					(c.attno == 0 && cols[i].attno == 0 &&
					 equal(cols[i].expr, c.expr)))
				{
					cols[i].is_eq = true;
					break;
				}
			if (i == ncols && ncols < maxcols)
				cols[ncols++] = c;
		}
	}
	return ncols;
}




/*
 * Candidate lattice: a candidate or a real plain-btree index is a node
 * (K = key columns, I = trailing/INCLUDE columns).  R covers C when C.K is
 * a prefix of R.K and C.I fits into R's remaining columns.  Coverage class
 * drives dedup uniformly:
 *   ADV_COV_EQUAL - same key list (duplicate by capability): always skip;
 *   ADV_COV_WIDER - strictly wider key list (heuristic: the wide index
 *                   serves the same quals at a page-width penalty): skip
 *                   only under advise_skip_prefix.
 * The reverse direction (candidate keys wider than any existing index) is
 * never reduced - that is a qualitatively different access.
 */
typedef enum AdvCoverage
{
	ADV_COV_NONE,
	ADV_COV_EQUAL,
	ADV_COV_WIDER
}			AdvCoverage;

static AdvCoverage
adv_coverage(RelOptInfo *rel, AttrNumber *attnos, int n, int nkeys)
{
	AdvCoverage best = ADV_COV_NONE;
	ListCell   *lc;

	foreach(lc, rel->indexlist)
	{
		IndexOptInfo *ioi = (IndexOptInfo *) lfirst(lc);
		int			i,
					j;

		if (ioi->relam != BTREE_AM_OID || ioi->hypothetical ||
			ioi->indexprs != NIL || ioi->indpred != NIL)
			continue;
		if (ioi->nkeycolumns < nkeys)
			continue;			/* candidate keys wider: never reduced */
		for (i = 0; i < nkeys; i++)
			if (ioi->indexkeys[i] != attnos[i])
				break;
		if (i < nkeys)
			continue;
		/* include part must fit into R's remaining columns */
		for (i = nkeys; i < n; i++)
		{
			for (j = nkeys; j < ioi->ncolumns; j++)
				if (ioi->indexkeys[j] == attnos[i])
					break;
			if (j == ioi->ncolumns)
				break;
		}
		if (i < n)
			continue;
		if (ioi->nkeycolumns == nkeys)
			return ADV_COV_EQUAL;
		best = ADV_COV_WIDER;
	}
	return best;
}

static bool
adv_cap_reached(ProposedPathState * state)
{
	int			cap = pathprobe_agent_mode ?
		Min(pathprobe_max_advisor_candidates, 16) :
		pathprobe_max_advisor_candidates;

	if (state->advise_generated >= cap)
	{
		state->advise_truncated = true;
		return true;
	}
	state->advise_generated++;
	return false;
}

/*
 * adv_skip_candidate - uniform dedup gate for all generation sites:
 * in-run exact signature, then lattice coverage by existing indexes.
 */
static bool
adv_skip_candidate(ProposedPathState * state, RelOptInfo *rel,
				   AttrNumber *attnos, int n, int nkeys)
{
	ListCell   *lc;
	AdvCoverage cov;

	foreach(lc, state->hypo)
	{
		HypoIndexSpec *h = (HypoIndexSpec *) lfirst(lc);
		int			i;

		if (h->adv_attnos == NULL || h->ncols != n ||
			(h->nkeys > 0 ? h->nkeys : h->ncols) != nkeys)
			continue;
		for (i = 0; i < n; i++)
			if (h->adv_attnos[i] != attnos[i])
				break;
		if (i == n)
			return true;
	}
	cov = adv_coverage(rel, attnos, n, nkeys);
	if (cov == ADV_COV_EQUAL)
		return true;
	if (cov == ADV_COV_WIDER && state->advise_skip_prefix)
		return true;
	return false;
}

static HypoIndexSpec *
adv_make_spec(RangeTblEntry *rte, const char *relname,
			  AttrNumber *attnos, int n, int nkeys, int seq)
{
	HypoIndexSpec *h = (HypoIndexSpec *) palloc0(sizeof(HypoIndexSpec));
	StringInfoData ddl;
	int			i;

	if (nkeys <= 0 || nkeys > n)
		nkeys = n;
	h->relname = pstrdup(relname);
	h->name = psprintf("adv_%d", seq);
	h->ncols = n;
	h->nkeys = nkeys;
	h->adv_attnos = (AttrNumber *) palloc(sizeof(AttrNumber) * n);
	memcpy(h->adv_attnos, attnos, sizeof(AttrNumber) * n);
	initStringInfo(&ddl);
	appendStringInfo(&ddl, "CREATE INDEX %s ON %s (", h->name, relname);
	for (i = 0; i < nkeys; i++)
	{
		h->colnames[i] = get_attname(rte->relid, attnos[i], false);
		appendStringInfo(&ddl, "%s%s", i ? ", " : "", h->colnames[i]);
	}
	appendStringInfoChar(&ddl, ')');
	for (i = nkeys; i < n; i++)
	{
		h->colnames[i] = get_attname(rte->relid, attnos[i], false);
		appendStringInfo(&ddl, "%s%s%s", i == nkeys ? " INCLUDE (" : ", ",
						 h->colnames[i], i == n - 1 ? ")" : "");
	}
	h->sql = ddl.data;			/* copy-pasteable DDL in the report */
	return h;
}

void
pathprobe_advise_generate(PlannerInfo *root, RelOptInfo *rel, RangeTblEntry *rte,
				ProposedPathState * state)
{
	AdvCol		cols[PATHPROBE_ADVISE_MAX];
	int			ncols = 0;
	AttrNumber	comp[PATHPROBE_MAX_HYPO_COLS];
	int			ncomp = 0;
	const char *relname = get_rel_name(rte->relid);
	int			seq = list_length(state->hypo) + 1;
	ListCell   *lc;
	int			i,
				j;

	if (!relname)
		return;
	if (pg_class_aclcheck(rte->relid, GetUserId(), ACL_SELECT) != ACLCHECK_OK)
		return;					/* silently skip rels the user cannot read */

	/*
	 * Collect index atoms: equality atoms from EquivalenceClasses first
	 * (the planner's own dedup, with transitivity), then remaining shapes
	 * from baserestrictinfo.  Uniqueness: attno for columns, equal() for
	 * expression atoms.
	 */
	ncols = adv_atoms_from_ecs(root, rel, cols, ncols,
							   PATHPROBE_ADVISE_MAX);
	foreach(lc, rel->baserestrictinfo)
	{
		RestrictInfo *rinfo = (RestrictInfo *) lfirst(lc);
		AdvCol		c;

		if (ncols >= PATHPROBE_ADVISE_MAX)
			break;
		if (!adv_col_btree_qual(rel, rte, rinfo, &c))
			continue;
		for (i = 0; i < ncols; i++)
			if ((c.attno > 0 && cols[i].attno == c.attno) ||
				(c.attno == 0 && cols[i].attno == 0 &&
				 equal(cols[i].expr, c.expr)))
			{
				cols[i].is_eq |= c.is_eq;
				break;
			}
		if (i == ncols)
			cols[ncols++] = c;
	}

	/* transparency: the atoms stage is printable */
	{
		StringInfoData ab;

		initStringInfo(&ab);
		for (i = 0; i < ncols; i++)
		{
			if (i > 0)
				appendStringInfoString(&ab, ", ");
			if (cols[i].attno > 0)
				appendStringInfoString(&ab,
									   get_attname(rte->relid, cols[i].attno, false));
			else
				appendStringInfo(&ab, "(%s)",
								 deparse_expression((Node *) cols[i].expr,
													deparse_context_for(relname, rte->relid),
													false, false));
			appendStringInfoString(&ab, cols[i].is_eq ? "=" : "~");
		}
		if (state->atoms_str && state->atoms_str[0])
			state->atoms_str = psprintf("%s | %s: %s",
										state->atoms_str, relname, ab.data);
		else
			state->atoms_str = psprintf("%s: %s", relname, ab.data);
	}

	/* single-column candidates: equalities first */
	for (j = 0; j < 2; j++)		/* pass 0: eq, pass 1: range */
		for (i = 0; i < ncols; i++)
		{
			AttrNumber	a[1];

			if (cols[i].is_eq != (j == 0) || cols[i].attno == 0)
				continue;
			a[0] = cols[i].attno;
			if (adv_skip_candidate(state, rel, a, 1, 1))
				continue;
			if (adv_cap_reached(state))
				break;
			state->hypo = lappend(state->hypo,
								  adv_make_spec(rte, relname, a, 1, 0, seq++));
		}

	/* one composite: all equalities + first range column (btree shape) */
	for (i = 0; i < ncols && ncomp < PATHPROBE_MAX_HYPO_COLS; i++)
		if (cols[i].is_eq && cols[i].attno > 0)
			comp[ncomp++] = cols[i].attno;
	for (i = 0; i < ncols && ncomp < PATHPROBE_MAX_HYPO_COLS; i++)
		if (!cols[i].is_eq && cols[i].attno > 0)
		{
			comp[ncomp++] = cols[i].attno;
			break;
		}
	if (ncomp >= 2 && !adv_skip_candidate(state, rel, comp, ncomp, ncomp) &&
		!adv_cap_reached(state))
		state->hypo = lappend(state->hypo,
							  adv_make_spec(rte, relname, comp, ncomp, 0, seq++));


	/* expression atoms: single-column expression candidates via the real
	 * CREATE INDEX path (deparse -> parser -> transformIndexStmt), which
	 * already handles indexprs/indextlist.  Composite with expressions is
	 * out of scope for v1 (named boundary). */
	for (i = 0; i < ncols; i++)
	{
		char	   *edd;
		HypoIndexSpec *h;

		if (cols[i].attno != 0)
			continue;
		edd = deparse_expression((Node *) cols[i].expr,
								 deparse_context_for(relname, rte->relid),
								 false, false);
		if (adv_cap_reached(state))
			break;
		h = hypo_spec_from_sql(psprintf("CREATE INDEX adv_%d ON %s ((%s))",
										seq, relname, edd), -1);
		if (h != NULL)
		{
			h->name = psprintf("adv_%d", seq++);
			state->hypo = lappend(state->hypo, h);
		}
	}

	/* covering candidate: best key set + INCLUDE of remaining query columns */
	{
		AttrNumber	cov[PATHPROBE_MAX_HYPO_COLS];
		int			nkey = 0;
		int			ntot;
		ListCell   *lt;

		if (ncomp >= 2)
		{
			memcpy(cov, comp, sizeof(AttrNumber) * ncomp);
			nkey = ncomp;
		}
		else
		{
			for (i = 0; i < ncols; i++)
				if (cols[i].attno > 0)
				{
					cov[0] = cols[i].attno;
					nkey = 1;
					break;
				}
		}
		ntot = nkey;
		if (nkey > 0)
		{
			foreach(lt, rel->reltarget->exprs)
			{
				Node	   *e = (Node *) lfirst(lt);
				AttrNumber	a;
				int			k;

				if (!IsA(e, Var) || ((Var *) e)->varno != rel->relid ||
					((Var *) e)->varattno <= 0)
					continue;
				a = ((Var *) e)->varattno;
				for (k = 0; k < ntot; k++)
					if (cov[k] == a)
						break;
				if (k < ntot)
					continue;
				if (ntot >= PATHPROBE_MAX_HYPO_COLS)
				{
					ntot = nkey;	/* does not fit: no covering candidate */
					break;
				}
				cov[ntot++] = a;
			}
		}
		if (ntot > nkey && !adv_skip_candidate(state, rel, cov, ntot, nkey) &&
			!adv_cap_reached(state))
			state->hypo = lappend(state->hypo,
								  adv_make_spec(rte, relname, cov, ntot,
												nkey, seq++));
	}

	/* ordered candidate from query_pathkeys, else group_pathkeys (ASC-only) */
	{
		AttrNumber	ord[PATHPROBE_MAX_HYPO_COLS];
		int			nord = 0;
		List	   *pks = root->query_pathkeys != NIL ?
			root->query_pathkeys : root->group_pathkeys;
		bool		ok = (pks != NIL);

		foreach(lc, pks)
		{
			PathKey    *pk = (PathKey *) lfirst(lc);
			EquivalenceMember *em = NULL;
			ListCell   *lm;

			if (pk->pk_cmptype != COMPARE_LT || pk->pk_nulls_first)
			{
				ok = false;		/* DESC/NULLS FIRST needs the stmt form; v1 skips */
				break;
			}
			foreach(lm, pk->pk_eclass->ec_members)
			{
				EquivalenceMember *m = (EquivalenceMember *) lfirst(lm);

				if (IsA(m->em_expr, Var) &&
					((Var *) m->em_expr)->varno == rel->relid &&
					((Var *) m->em_expr)->varattno > 0)
				{
					em = m;
					break;
				}
			}
			if (!em || nord >= PATHPROBE_MAX_HYPO_COLS)
			{
				ok = false;
				break;
			}
			ord[nord++] = ((Var *) em->em_expr)->varattno;
		}
		if (ok && nord > 0 && !adv_skip_candidate(state, rel, ord, nord, nord) &&
			!adv_cap_reached(state))
			state->hypo = lappend(state->hypo,
								  adv_make_spec(rte, relname, ord, nord, 0, seq++));
	}
}
