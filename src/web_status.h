#ifndef WEB_STATUS_H
#define WEB_STATUS_H

#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include <pthread.h>

/* v1 status server: read-only, GET /status.json only - no admin/control
 * routes yet (that's Phase 3, once there's a web UI that needs to POST
 * graph changes). Same accept-loop-on-a-pthread shape as
 * dpdk-app-example's web_status.c (a small poll()-with-timeout loop, not
 * a busy loop, so it notices shutdown promptly without blocking
 * indefinitely in accept()), trimmed down to just what v1 needs. */
struct app_web_status {
	time_t start_time;
	pthread_mutex_t lock;
	uint64_t records_in;
	uint64_t records_dropped;
	uint64_t records_forwarded;
};

void app_web_status_init(struct app_web_status *status);
void app_web_status_destroy(struct app_web_status *status);

/* Snapshots the given counters under status->lock - called periodically
 * from main.c's loop (not from the hot pipeline_run() path itself, so
 * there's no lock contention on the datapath). */
void app_web_status_update(struct app_web_status *status, uint64_t records_in,
			    uint64_t records_dropped, uint64_t records_forwarded);

int web_status_start(uint16_t web_port, struct app_web_status *status, volatile bool *quit_flag);
void web_status_stop(void);

#endif
