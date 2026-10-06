/*-------------------------------------------------------------------------
 * pathprobe_json.c
 * Part of pathprobe.  Distributed under the PostgreSQL License;
 * see the repository LICENSE file.
 *-------------------------------------------------------------------------
 */
/*
 * pathprobe_json.c - JSON renderer (package v1.2)
 *
 * render_json(): same data as render_text(), machine-readable.
 * pathprobe_json(): SQL wrapper returning text (valid JSON).
 *
 * Output structure:
 * {
 *   "version": "1.1",     -- output-format version, NOT the package version
 *   "rels": {"1": "t", "2": "s"},
 *   "events": [
 *     {"seq":1, "src":"p1", "rel":"{1}", "pathtype":"BitmapHeapScan",
 *      "cost_total":11.27, "kind":"rejected"},
 *     ...
 *   ],
 *   "summary": [
 *     {"rel":"{1,2}", "strategy":"NestLoop",
 *      "excluded":0, "skipped":5, "rejected":1, "accepted":0, "survived":0},
 *     ...
 *   ]
 * }
 */

#include "postgres.h"

#include "fmgr.h"
#include "lib/stringinfo.h"
#include "nodes/bitmapset.h"
#include "utils/builtins.h"
#include "utils/memutils.h"

#include "pathprobe.h"

/* single JSON string sink: full escaping lives in pathprobe_json_str() */
static void
json_str(StringInfo out, const char *s)
{
	pathprobe_json_str(out, s);
}

void
render_json(PathProbeReport *report, StringInfo out, PathProbeOpts *opts,
			const char *query_string)
{
	PathEventLog *log = report->log;
	PathSummary *sum;
	int			i;
	bool		first;

	appendStringInfoString(out, "{\n");

	/* version */
	appendStringInfoString(out, "  \"version\": \"1.1\",\n");
	if (query_string)
	{
		appendStringInfoString(out, "  \"query\": ");
		json_str(out, query_string);
		appendStringInfoString(out, ",\n");
	}

	/* rels legend */
	appendStringInfoString(out, "  \"rels\": {");
	if (log->rtmap)
	{
		int			ri;
		bool		first_rel = true;

		for (ri = 0; ri < log->rtmap->nentries; ri++)
		{
			char		keybuf[16];

			if (!log->rtmap->names[ri])
				continue;
			if (!first_rel)
				appendStringInfoChar(out, ',');
			first_rel = false;
			snprintf(keybuf, sizeof(keybuf), "%d", ri + 1);
			json_str(out, keybuf);
			appendStringInfoChar(out, ':');
			json_str(out, log->rtmap->names[ri]);
		}
	}
	appendStringInfoString(out, "},\n");

	/* events array */
	appendStringInfoString(out, "  \"events\": [\n");
	first = true;
	for (i = 0; i < log->nevents; i++)
	{
		PathEvent  *ev = &log->events[i];
		char		rel_buf[32];
		const char *kind_str;
		const char *src_str;

		switch (ev->kind)
		{
			case PEK_EXCLUDED:
				kind_str = "excluded";
				break;
			case PEK_SKIPPED:
				kind_str = "skipped";
				break;
			case PEK_REJECTED:
				kind_str = "rejected";
				break;
			case PEK_ACCEPTED:
				kind_str = "accepted";
				break;
			case PEK_DISPLACED:
				kind_str = "displaced";
				break;
			case PEK_REMOVED:
				kind_str = "removed";
				break;
			case PEK_SURVIVED:
				kind_str = "survived";
				break;
			default:
				kind_str = "unknown";
				break;
		}
		switch (ev->source)
		{
			case PATH_SOURCE_PRECHECK:
				src_str = "pc";
				break;
			case PATH_SOURCE_ADD_PATH:
				src_str = "p1";
				break;
			case PATH_SOURCE_REL_HOOK:
				src_str = "p0";
				break;
			case PATH_SOURCE_JOIN_HOOK:
				src_str = "j2";
				break;
			case PATH_SOURCE_PROPOSED:
				src_str = "wp";
				break;
			default:
				src_str = "?";
				break;
		}

		if (ev->relids && !bms_is_empty(ev->relids))
			relids_to_str(ev->relids, rel_buf, sizeof(rel_buf));
		else
			strlcpy(rel_buf, "?", sizeof(rel_buf));

		if (!first)
			appendStringInfoString(out, ",\n");
		first = false;

		appendStringInfoString(out, "    {");
		appendStringInfo(out, "\"seq\":%d,", ev->seq);
		appendStringInfoString(out, "\"src\":");
		json_str(out, src_str);
		appendStringInfoString(out, ",\"rel\":");
		json_str(out, rel_buf);
		appendStringInfoString(out, ",\"pathtype\":");
		json_str(out, pathtype_name(ev->pathtype));
		appendStringInfo(out, ",\"cost_total\":%.2f", ev->cost_total);
		appendStringInfoString(out, ",\"kind\":");
		json_str(out, kind_str);
		if (ev->path_id > 0)
			appendStringInfo(out, ",\"path_id\":%d", ev->path_id);
		else
			appendStringInfoString(out, ",\"path_id\":null");
		if (ev->kind == PEK_DISPLACED)
		{
			if (ev->link_path_id > 0)
				appendStringInfo(out, ",\"displacer_path_id\":%d",
								 ev->link_path_id);
			else
				appendStringInfoString(out, ",\"displacer_path_id\":null");
			if (ev->link_seq > 0)
				appendStringInfo(out, ",\"displacer_seq\":%d", ev->link_seq);
			else
				appendStringInfoString(out, ",\"displacer_seq\":null");
			appendStringInfoString(out, ",\"other_pathtype\":");
			json_str(out, pathtype_name(ev->other_pathtype));
		}
		else if (ev->kind == PEK_REJECTED)
		{
			/* the old path add_path() found to dominate this one */
			if (ev->link_path_id > 0)
			{
				appendStringInfo(out, ",\"dominator_path_id\":%d",
								 ev->link_path_id);
				appendStringInfoString(out, ",\"dominator_pathtype\":");
				json_str(out, pathtype_name(ev->link_pathtype));
			}
			else
				appendStringInfoString(out, ",\"dominator_path_id\":null"
									   ",\"dominator_pathtype\":null");
			if (ev->link_seq > 0)
				appendStringInfo(out, ",\"dominator_seq\":%d", ev->link_seq);
			else
				appendStringInfoString(out, ",\"dominator_seq\":null");
		}
		else if (ev->kind == PEK_REMOVED)
		{
			/* the new path that removed this one; add_path() rejected it */
			if (ev->link_path_id > 0)
				appendStringInfo(out, ",\"remover_path_id\":%d",
								 ev->link_path_id);
			else
				appendStringInfoString(out, ",\"remover_path_id\":null");
			if (ev->link_seq > 0)
				appendStringInfo(out, ",\"remover_seq\":%d", ev->link_seq);
			else
				appendStringInfoString(out, ",\"remover_seq\":null");
			appendStringInfoString(out, ",\"remover_pathtype\":");
			json_str(out, pathtype_name(ev->link_pathtype));
		}
		if (ev->label)
		{
			appendStringInfoString(out, ",\"label\":");
			json_str(out, ev->label);
		}
		if (ev->kept_reason)
		{
			appendStringInfoString(out, ",\"kept_reason\":");
			json_str(out, ev->kept_reason);
		}
		appendStringInfo(out, ",\"origin\":\"%s\"",
						 (ev->label != NULL ||
						  ev->source == PATH_SOURCE_PROPOSED) ?
						 "proposed" : "real");

		if (ev->gap_cost != 0.0)
			appendStringInfo(out, ",\"gap_cost\":%.2f", ev->gap_cost);

		if (ev->kind == PEK_SKIPPED)
		{
			const char *reason =
				(ev->skip_reason == SKIP_DISABLED) ? "disabled" :
				(ev->skip_reason == SKIP_PARAM_MISMATCH) ? "param_mismatch" :
				(ev->skip_reason == SKIP_DOMINATED_EARLY) ? "dominated_early" : "unknown";

			appendStringInfoString(out, ",\"skip_reason\":");
			json_str(out, reason);
		}

		if (ev->is_parameterized)
			appendStringInfoString(out, ",\"param\":true");

		if (ev->disabled_nodes > 0)
			appendStringInfo(out, ",\"disabled_nodes\":%d", ev->disabled_nodes);

		appendStringInfoChar(out, '}');
	}
	appendStringInfoString(out, "\n  ],\n");

	/* path nodes: identity and child edges of every path_id above */
	appendStringInfoString(out, "  \"paths\": [");
	for (i = 0; i < log->nnodes; i++)
	{
		PathNode   *n = &log->nodes[i];
		char		rel_buf[32];

		if (n->relids && !bms_is_empty(n->relids))
			relids_to_str(n->relids, rel_buf, sizeof(rel_buf));
		else
			strlcpy(rel_buf, "?", sizeof(rel_buf));
		appendStringInfo(out, "%s\n    {\"path_id\":%d,\"pathtype\":",
						 i > 0 ? "," : "", n->id);
		json_str(out, pathtype_name(n->pathtype));
		appendStringInfoString(out, ",\"rel\":");
		json_str(out, rel_buf);
		if (n->subpath_id > 0)
			appendStringInfo(out, ",\"subpath_id\":%d", n->subpath_id);
		if (n->outer_path_id > 0)
			appendStringInfo(out, ",\"outer_path_id\":%d", n->outer_path_id);
		if (n->inner_path_id > 0)
			appendStringInfo(out, ",\"inner_path_id\":%d", n->inner_path_id);
		if (n->has_children)
		{
			appendStringInfoString(out, ",\"child_path_ids\":[");
			for (int c = 0; c < n->nchildren; c++)
				appendStringInfo(out, c ? ",%d" : "%d", n->child_ids[c]);
			appendStringInfoChar(out, ']');
		}
		appendStringInfoChar(out, '}');
	}
	appendStringInfoString(out, log->nnodes > 0 ? "\n  ],\n" : "],\n");

	/* summary array */
	sum = pelog_build_summary(log, CurrentMemoryContext);
	appendStringInfo(out,
					 "  \"limits\": {\"events_truncated\":%s,"
					 "\"summary_truncated\":%s,"
					 "\"max_events\":%d,\"max_summary_entries\":%d},\n",
					 log->events_truncated ? "true" : "false",
					 sum->truncated ? "true" : "false",
					 pathprobe_max_events, pathprobe_max_summary_entries);
#define COV(bit) ((log->coverage & (bit)) ? "true" : "false")
	appendStringInfo(out,
					 "  \"coverage\": {\"survived_snapshot\":%s,"
					 "\"add_path_accepted\":%s,\"add_path_rejected\":%s,"
					 "\"add_path_displaced\":%s,\"add_path_removed\":%s,"
					 "\"add_path_precheck\":%s,"
					 "\"join_excluded\":%s,\"index_build\":%s},\n",
					 COV(PP_COV_SURVIVED), COV(PP_COV_ACCEPTED),
					 COV(PP_COV_REJECTED), COV(PP_COV_DISPLACED),
					 COV(PP_COV_REMOVED),
					 COV(PP_COV_PRECHECK), COV(PP_COV_EXCLUDED),
					 COV(PP_COV_INDEX_BUILD));
#undef COV
	appendStringInfoString(out, "  \"summary\": [\n");
	first = true;
	for (i = 0; i < sum->nentries; i++)
	{
		PathSummaryEntry *ent = &sum->entries[i];
		char		rel_buf[32];

		relids_to_str(ent->relids, rel_buf, sizeof(rel_buf));

		if (!first)
			appendStringInfoString(out, ",\n");
		first = false;

		appendStringInfoString(out, "    {");
		appendStringInfoString(out, "\"rel\":");
		json_str(out, rel_buf);
		appendStringInfoString(out, ",\"strategy\":");
		json_str(out, pathtype_name(ent->pathtype));
		appendStringInfo(out,
						 ",\"excluded\":%d,\"skipped\":%d,\"rejected\":%d"
						 ",\"accepted\":%d,\"survived\":%d",
						 ent->n_excluded, ent->n_skipped, ent->n_rejected,
						 ent->n_accepted, ent->n_survived);

		if (ent->has_winner)
			appendStringInfo(out, ",\"best_cost\":%.2f", ent->best_cost);

		appendStringInfoChar(out, '}');
	}
	appendStringInfoString(out, "\n  ]\n}\n");
	pfree(sum);
}

/* -------------------------------------------------------------------------
 * SQL wrapper: pathprobe(query text) RETURNS text
 * -------------------------------------------------------------------------
 */
PG_FUNCTION_INFO_V1(pathprobe);

Datum
pathprobe(PG_FUNCTION_ARGS)
{
	text	   *query_text = PG_GETARG_TEXT_PP(0);
	char	   *query_string = text_to_cstring(query_text);

	PathProbeOpts opts = {false, false};
	PathProbeReport *report;
	StringInfoData out;

	if (strlen(query_string) > (size_t) pathprobe_max_query_length)
		ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
						errmsg("pathprobe: query longer than pathprobe.max_query_length (%d)",
							   pathprobe_max_query_length)));

	report = pathprobe_internal(query_string, &opts);

	initStringInfo(&out);
	render_text(report, &out, &opts);

	PG_RETURN_TEXT_P(cstring_to_text(out.data));
}

/* -------------------------------------------------------------------------
 * SQL wrapper: pathprobe_json(query text) RETURNS text
 * -------------------------------------------------------------------------
 */
PG_FUNCTION_INFO_V1(pathprobe_json);

Datum
pathprobe_json(PG_FUNCTION_ARGS)
{
	text	   *query_text = PG_GETARG_TEXT_PP(0);
	char	   *query_string = text_to_cstring(query_text);

	PathProbeOpts opts = {false, false};
	PathProbeReport *report;
	StringInfoData out;

	if (strlen(query_string) > (size_t) pathprobe_max_query_length)
		ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
						errmsg("pathprobe: query longer than pathprobe.max_query_length (%d)",
							   pathprobe_max_query_length)));

	report = pathprobe_internal(query_string, &opts);

	initStringInfo(&out);
	render_json(report, &out, &opts, query_string);

	PG_RETURN_TEXT_P(cstring_to_text(out.data));
}
