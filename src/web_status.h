#ifndef WEB_STATUS_H
#define WEB_STATUS_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <time.h>
#include <pthread.h>

/* Status/API server: GET /status.json, GET /api/stage-types (Phase 3 -
 * serializes stage_registry.c's table for the web UI's palette),
 * GET /api/graph (the last-saved graph's raw JSON, for the UI to load on
 * open), POST /api/graph (validates a new graph exactly as startup does
 * and, on success, writes it to --graph=PATH's file - it does NOT
 * hot-swap the running pipeline; the response says so, and the process
 * needs a restart to actually pick it up). CLAUDE.md's Phase 3 design
 * sketch flagged live-reload-vs-restart as an open decision; this
 * resolves it in favor of restart, deliberately - hot-swapping the
 * running pipeline_chain would mean sharing it with epoch_barrier.c's
 * worker-pool synchronization, which is real, correctness-critical,
 * already-subtle code not worth destabilizing for this. Static files
 * out of --web-root (Phase 3's built web/dist/) round out the routes.
 * Same accept-loop-on-a-pthread shape as dpdk-app-example's
 * web_status.c (a small poll()-with-timeout loop, not a busy loop, so
 * it notices shutdown promptly without blocking indefinitely in
 * accept()). */
struct app_web_status {
	time_t start_time;
	pthread_mutex_t lock;
	uint64_t records_in;
	uint64_t records_dropped;
	uint64_t records_forwarded;
};

/* Everything GET/POST /api/graph need: where to write a validated graph
 * (graph_path - the same file --graph=PATH loaded at startup) and the
 * raw text of the last graph that was successfully saved (initially:
 * that same startup file) so GET /api/graph has something to serve.
 * Not const: current_graph_json is mutated in place (old copy freed,
 * replaced) each time a save succeeds. Pass a NULL ctx pointer to
 * web_status_start() to disable both routes entirely (501) - e.g. for a
 * future test harness that doesn't care about the graph API. */
struct web_graph_ctx {
	const char *graph_path;     /* not copied - must outlive the server */
	char *current_graph_json;   /* malloc'd; web_status.c owns mutating it */
	size_t current_graph_len;
};

void app_web_status_init(struct app_web_status *status);
void app_web_status_destroy(struct app_web_status *status);

/* Snapshots the given counters under status->lock - called periodically
 * from main.c's loop (not from the hot pipeline_run() path itself, so
 * there's no lock contention on the datapath). */
void app_web_status_update(struct app_web_status *status, uint64_t records_in,
			    uint64_t records_dropped, uint64_t records_forwarded);

/* web_root: directory holding the built web/dist/ (Phase 3 UI) to serve
 * as static files, or NULL/"" to disable static-file serving (status.json
 * and stage-types still work either way). Not copied - must outlive the
 * server (main.c passes a string literal / argv-derived pointer that
 * lives for the process lifetime).
 * graph_ctx: see struct web_graph_ctx above; NULL disables
 * GET/POST /api/graph (501). Not copied either - main.c's copy must
 * outlive the server, and web_status.c mutates its current_graph_json
 * field in place on every successful save. */
int web_status_start(uint16_t web_port, struct app_web_status *status, volatile bool *quit_flag,
		      const char *web_root, struct web_graph_ctx *graph_ctx);
void web_status_stop(void);

#endif
