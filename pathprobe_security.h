/*-------------------------------------------------------------------------
 * pathprobe_security.h
 * ACL-aware access control for pathprobe propose/advisor surfaces.
 * Part of pathprobe.  Distributed under the PostgreSQL License;
 * see the repository LICENSE file.
 *-------------------------------------------------------------------------
 */
#ifndef PATHPROBE_SECURITY_H
#define PATHPROBE_SECURITY_H

#include "postgres.h"
#include "nodes/parsenodes.h"
#include "nodes/pg_list.h"

/*
 * Access decision for a probe request.  We do not build a parallel privilege
 * model: relation/column SELECT is delegated to core ExecCheckPermissions()
 * over the rewritten Query's rteperminfos; this struct only carries the
 * derived facts the propose/advisor/hypo layers need for their own gating.
 */
typedef struct PathProbeAccess
{
	bool		is_superuser;	/* caller is superuser: full behaviour */
	Bitmapset  *allowed_relids; /* base relation OIDs the caller may inspect */
} PathProbeAccess;

/*
 * Authorize a probe request.  querytree_list is the analyzed+rewritten query
 * (views expanded, rteperminfos populated).  For a non-superuser this:
 *   - requires exactly one SELECT query,
 *   - runs core relation/column SELECT checks (ExecCheckPermissions),
 *   - fails closed on RLS-enabled relations,
 *   - fails closed if a rewritten RTE would be checked as another role
 *     (view/rule exposing a base relation the caller cannot select),
 *   - populates access->allowed_relids with the vetted base relations.
 * Superusers keep current behaviour (allowed_relids left NULL = "all").
 * Raises ERROR on any violation; never redacts.
 */
extern void pathprobe_authorize_query(List *querytree_list,
									  PathProbeAccess *access);

/*
 * Spec-level guard for a hypothetical index the caller wants to inject.
 * For non-superusers: structured hypo only (checked by the caller before
 * calling here), relation must be in allowed_relids, btree only.  hypo_sql
 * is rejected for non-superusers at the call site.  Raises ERROR on
 * violation.
 */
extern void pathprobe_check_hypo_relation(PathProbeAccess *access,
										  Oid relid, Oid amoid,
										  bool from_sql);

/*
 * Output guard: assert a relation named in an event/candidate/advisor entry
 * is in the vetted set.  For superusers this is a no-op.  Raises ERROR on a
 * relation outside allowed_relids (defence in depth against a leak).
 */
extern void pathprobe_assert_relation_allowed(PathProbeAccess *access,
											  Oid relid);

#endif							/* PATHPROBE_SECURITY_H */
