/*-------------------------------------------------------------------------
 * pathprobe_security.c
 * ACL-aware access control for the pathprobe propose/advisor surfaces.
 *
 * Design (per @yoda security review): do NOT invent a parallel privilege
 * model.  Delegate relation/column SELECT to core ExecCheckPermissions()
 * over the rewritten Query's rteperminfos, exactly as the executor would.
 * The functions themselves stay SECURITY INVOKER, so GetUserId() is the
 * caller.  Superusers keep current behaviour; non-superusers are held to a
 * conservative, fail-closed subset (single SELECT, no RLS, no view/rule
 * exposing base relations they cannot select, structured btree hypo only).
 *
 * Part of pathprobe.  Distributed under the PostgreSQL License.
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "catalog/pg_am_d.h"
#include "catalog/pg_class.h"
#include "executor/executor.h"
#include "miscadmin.h"
#include "nodes/nodes.h"
#include "utils/acl.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/syscache.h"

#include "pathprobe_security.h"

static void authorize_query_recurse(Query *query, Oid caller,
									PathProbeAccess *access);

/* does this relation have row-level security enabled (catalog flag)? */
static bool
rel_has_row_security(Oid relid)
{
	HeapTuple	tp;
	bool		result = false;

	tp = SearchSysCache1(RELOID, ObjectIdGetDatum(relid));
	if (HeapTupleIsValid(tp))
	{
		Form_pg_class classform = (Form_pg_class) GETSTRUCT(tp);

		result = classform->relrowsecurity;
		ReleaseSysCache(tp);
	}
	return result;
}

/*
 * Authorize the analyzed+rewritten query.  See header for the policy.
 */
void
pathprobe_authorize_query(List *querytree_list, PathProbeAccess *access)
{
	Query	   *query;
	Oid			caller = GetUserId();

	access->is_superuser = superuser();
	access->allowed_relids = NULL;

	/* Superuser: keep current behaviour, no relation restriction. */
	if (access->is_superuser)
		return;

	/*
	 * Non-superuser: the statement must analyze to exactly one query.  More
	 * than one here means a rule expanded the statement (e.g. DO ALSO); we do
	 * not vet rule-expanded query sets for non-superusers.  (Multiple input
	 * statements are already rejected before analysis by the caller.)
	 */
	if (querytree_list == NIL || list_length(querytree_list) != 1)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("pathprobe: non-superuser probes must be a single query")));

	query = linitial_node(Query, querytree_list);

	if (query->commandType != CMD_SELECT)
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("pathprobe: non-superuser probes are limited to SELECT")));

	/*
	 * Walk the whole query tree (top level plus every subquery produced by
	 * view/rule rewriting) and authorize each level.  A plain view expands
	 * into a subquery RTE whose base relation is not present in the outer
	 * query's rteperminfos, so a single-level check would miss it.
	 */
	authorize_query_recurse(query, caller, access);
}

/*
 * Authorize one Query level and recurse into its subqueries.
 *   - core relation/column SELECT via ExecCheckPermissions,
 *   - fail closed on RLS and on any RTE checked as another role,
 *   - collect every RTE_RELATION relid into access->allowed_relids.
 */
static void
authorize_query_recurse(Query *query, Oid caller, PathProbeAccess *access)
{
	ListCell   *lc;

	if (query->commandType != CMD_SELECT)
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("pathprobe: non-superuser probes are limited to SELECT")));

	(void) ExecCheckPermissions(query->rtable, query->rteperminfos, true);

	foreach(lc, query->rteperminfos)
	{
		RTEPermissionInfo *perm = lfirst_node(RTEPermissionInfo, lc);

		if (OidIsValid(perm->checkAsUser) && perm->checkAsUser != caller)
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("pathprobe: cannot probe relations reached through another role's privileges"),
					 errdetail("A view or rule would expose a base relation you cannot select directly."),
					 errhint("This is restricted for non-superusers in this release.")));

		if (rel_has_row_security(perm->relid))
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("pathprobe: cannot probe relations with row-level security"),
					 errdetail("RLS-enabled relation \"%s\" is restricted for non-superusers in this release.",
							   get_rel_name(perm->relid))));
	}

	foreach(lc, query->rtable)
	{
		RangeTblEntry *rte = lfirst_node(RangeTblEntry, lc);

		if (rte->rtekind == RTE_RELATION && OidIsValid(rte->relid))
		{
			/*
			 * SELECT (column-aware) was already vetted by
			 * ExecCheckPermissions at this level; the checkAsUser guard above
			 * catches base relations reached as a view owner.  Here we only
			 * add RLS fail-closed and record the relation.
			 */
			if (rel_has_row_security(rte->relid))
				ereport(ERROR,
						(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
						 errmsg("pathprobe: cannot probe relations with row-level security"),
						 errdetail("RLS-enabled relation \"%s\" is restricted for non-superusers in this release.",
								   get_rel_name(rte->relid))));

			access->allowed_relids = bms_add_member(access->allowed_relids,
													rte->relid);
		}
		else if (rte->rtekind == RTE_SUBQUERY && rte->subquery != NULL)
			authorize_query_recurse(rte->subquery, caller, access);
	}
}

void
pathprobe_check_hypo_relation(PathProbeAccess *access, Oid relid, Oid amoid,
							  bool from_sql)
{
	if (access->is_superuser)
		return;

	/* hypo_sql is superuser-only for now (arbitrary DDL grammar). */
	if (from_sql)
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("pathprobe: hypo_sql is restricted to superusers in this release"),
				 errhint("Use structured hypo_indexes on relations you can select.")));

	/* Structured hypo must target a vetted relation. */
	if (!bms_is_member(relid, access->allowed_relids))
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("pathprobe: hypothetical index targets a relation not in the probed query")));

	/* Non-superusers: btree only initially. */
	if (OidIsValid(amoid) && amoid != BTREE_AM_OID)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("pathprobe: non-superuser hypothetical indexes are limited to btree")));
}

void
pathprobe_assert_relation_allowed(PathProbeAccess *access, Oid relid)
{
	if (access->is_superuser)
		return;
	if (!OidIsValid(relid))
		return;
	if (!bms_is_member(relid, access->allowed_relids))
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("pathprobe: internal: output references relation %u outside the authorized set",
						relid)));
}
