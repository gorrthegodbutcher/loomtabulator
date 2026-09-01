#ifndef WEB_STATUS_H
#define WEB_STATUS_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <time.h>
#include <pthread.h>
#include "pipeline.h"

/* Status/API server: GET /status.json, GET /api/stage-types (Phase 3 -
 * serializes plugin_loader.c's dynamically-populated registry for the
 * web UI's palette), GET /api/graph (the last-saved graph's raw JSON,
 * for the UI to load on open), POST /api/graph (validates a new graph
 * exactly as startup does and, on success, writes it to --graph=PATH's
 * file - it does NOT hot-swap the running pipeline; the response says
 * so), POST /api/reload (see below), GET /api/stage-status (each
 * node's live struct stage.get_status() snapshot, if it has one - see
 * struct stage_status_snapshot below), POST /api/probe-port-count
 * (builds one throwaway stage instance from a {"type","config"} body
 * and reports its out_port_count() - the web UI's only way to know how
 * many handles a multi-port node needs, since that's an instance
 * property, not something GET /api/stage-types' per-type listing can
 * answer). GET /api/graphs (list the named graphs in --graphs-dir),
 * POST /api/graphs?name=X (validate and add one to that directory,
 * without touching the active graph), POST /api/graphs/load?name=X
 * (validate one already in that directory and activate it - same
 * "writes to graph_path, restart_required: true" effect POST /api/graph
 * has), and DELETE /api/graphs?name=X (remove one from that directory)
 * round out a small library of graphs to switch between, on top of the
 * single active graph_path/current_graph_json above. CLAUDE.md's Phase 3
 * design sketch flagged live-reload-vs-
 * restart as an open decision; this resolves it in favor of restart,
 * deliberately - hot-swapping the running pipeline_chain would mean
 * sharing it with epoch_barrier.c's worker-pool synchronization, which
 * is real, correctness-critical, already-subtle code not worth
 * destabilizing for this. "Restart" doesn't mean "the operator has to
 * do it by hand" though: POST /api/reload triggers the exact same
 * graceful shutdown SIGINT does (drain the ring, join every worker,
 * tear down every stage, rte_eal_cleanup()), then main.c re-exec's
 * itself into a fresh instance with the newly-saved graph - see
 * main.c's own g_reload_requested comment for the full mechanism. Same
 * PID throughout (no fork), so there's no parent process left to
 * become a zombie, and no external supervisor is needed. Static files
 * out of --web-root (Phase 3's built web/dist/) round out the routes.
 * Same accept-loop-on-a-pthread shape as dpdk-app-example's
 * web_status.c (a small poll()-with-timeout loop, not a busy loop, so
 * it notices shutdown promptly without blocking indefinitely in
 * accept()). */
/* One node instance's status.h struct stage_status, plus enough to
 * identify which node/stage type it came from for GET /api/stage-status'
 * JSON - see app_web_status_update_stage_statuses() below for how this
 * gets filled. status.field_count == 0 means this stage either has no
 * get_status() at all, or has one that currently reports nothing -
 * both are normal, not errors. */
struct stage_status_snapshot {
	const char *node_id;
	const char *stage_type;
	struct stage_status status;
};

struct app_web_status {
	time_t start_time;
	pthread_mutex_t lock;
	uint64_t records_in;
	uint64_t records_dropped;
	uint64_t records_forwarded;

	/* Filled periodically by app_web_status_update_stage_statuses()
	 * below (main.c calls it on its own, slower --status-poll-interval
	 * cadence, separate from the records_in/dropped/forwarded update
	 * above) - GET /api/stage-status serves exactly this, under the
	 * same lock. Indexed 0..stage_status_count, not by chain position -
	 * a stage_status_count of 0 just means the collector hasn't run yet
	 * (e.g. right at startup, before the first --status-poll-interval
	 * tick). */
	struct stage_status_snapshot stage_statuses[PIPELINE_MAX_STAGES];
	size_t stage_status_count;
};

/* Everything GET/POST /api/graph need: where to write a validated graph
 * (graph_path - the same file --graph=PATH loaded at startup) and the
 * raw text of the last graph that was successfully saved (initially:
 * that same startup file) so GET /api/graph has something to serve.
 * Not const: current_graph_json is mutated in place (old copy freed,
 * replaced) each time a save succeeds. Pass a NULL ctx pointer to
 * web_status_start() to disable both routes entirely (501) - e.g. for a
 * future test harness that doesn't care about the graph API.
 *
 * graphs_dir rounds this out with the *library* of named graphs (see
 * GET/POST /api/graphs, POST /api/graphs/load, DELETE /api/graphs) - a
 * directory of standalone .json files, distinct from graph_path/
 * current_graph_json above (which always describe the single *active*
 * graph). Loading one from the library copies its bytes into graph_path
 * exactly the way POST /api/graph already does; the library itself is
 * otherwise untouched by activation. Not copied - must outlive the
 * server, same contract as graph_path. */
struct web_graph_ctx {
	const char *graph_path;     /* not copied - must outlive the server */
	char *current_graph_json;   /* malloc'd; web_status.c owns mutating it */
	size_t current_graph_len;
	const char *graphs_dir;     /* not copied - must outlive the server */
};

void app_web_status_init(struct app_web_status *status);
void app_web_status_destroy(struct app_web_status *status);

/* Snapshots the given counters under status->lock - called periodically
 * from main.c's loop (not from the hot pipeline_run() path itself, so
 * there's no lock contention on the datapath). */
void app_web_status_update(struct app_web_status *status, uint64_t records_in,
			    uint64_t records_dropped, uint64_t records_forwarded);

/* Calls chain->stages[i].stage->get_status() for every node that has
 * one (leaves stage_statuses[i].status.field_count at 0 for a node that
 * doesn't) and stores the results under status->lock, for
 * GET /api/stage-status to serve. Called from main.c's own status loop
 * on its own --status-poll-interval cadence (default 2s -
 * deliberately much slower than the records_in/dropped/forwarded
 * update above, since this is explicitly not a hot-path concern - see
 * struct stage.get_status's own comment in stage.h), never from a
 * worker lcore. */
void app_web_status_update_stage_statuses(struct app_web_status *status,
					   const struct pipeline_chain *chain);

/* web_root: directory holding the built web/dist/ (Phase 3 UI) to serve
 * as static files, or NULL/"" to disable static-file serving (status.json
 * and stage-types still work either way). Not copied - must outlive the
 * server (main.c passes a string literal / argv-derived pointer that
 * lives for the process lifetime).
 * graph_ctx: see struct web_graph_ctx above; NULL disables
 * GET/POST /api/graph (501). Not copied either - main.c's copy must
 * outlive the server, and web_status.c mutates its current_graph_json
 * field in place on every successful save. */
/* reload_flag: set alongside *quit_flag by POST /api/reload's handler -
 * main.c consults it after the ordinary shutdown sequence completes to
 * decide "re-exec a fresh instance" vs. "exit for good" (see
 * main.c's own g_reload_requested comment). */
int web_status_start(uint16_t web_port, struct app_web_status *status, volatile bool *quit_flag,
		      volatile bool *reload_flag, const char *web_root, struct web_graph_ctx *graph_ctx);
void web_status_stop(void);

#endif
