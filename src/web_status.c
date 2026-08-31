#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <unistd.h>
#include <inttypes.h>
#include <poll.h>
#include <dirent.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include "web_status.h"
#include "plugin_loader.h"
#include "graph_config.h"
#include "pipeline.h"
#include "json.h"

#define REQUEST_HEADER_BUF_SIZE 8192
#define MAX_GRAPH_BODY_BYTES (256 * 1024)
#define JSON_BUF_SIZE 512
/* Sized for up to PLUGIN_REGISTRY_MAX (plugin_loader.h) stage types,
 * not the fixed 4 built-ins this used to always be - handle_stage_types()
 * is snprintf()-bounded so it can't overflow this buffer, but it WILL
 * silently truncate into invalid JSON if this constant doesn't keep up
 * with the real registry cap. ~128 bytes/entry is generous for a
 * "name"/in_type/out_type triple, so 64 * 128 + slack rounds up to: */
#define STAGE_TYPES_JSON_BUF_SIZE 16384
#define ACCEPT_POLL_TIMEOUT_MS 1000
#define WEB_ROOT_MAX_PATH 512
#define STATIC_FILE_MAX_BYTES (5 * 1024 * 1024)
#define GRAPHS_DIR_MAX_PATH 512
#define GRAPH_NAME_MAX_LEN 128
#define MAX_GRAPHS_LISTED 256
#define GRAPHS_LIST_JSON_BUF_SIZE (MAX_GRAPHS_LISTED * 192)

struct web_server_ctx {
	int listen_fd;
	struct app_web_status *status;
	volatile bool *quit_flag;
	volatile bool *reload_flag;
	const char *web_root;
	struct web_graph_ctx *graph_ctx;
};

static pthread_t g_server_thread;
static bool g_server_running;
static struct web_server_ctx g_ctx;

void
app_web_status_init(struct app_web_status *status)
{
	memset(status, 0, sizeof(*status));
	status->start_time = time(NULL);
	pthread_mutex_init(&status->lock, NULL);
}

void
app_web_status_destroy(struct app_web_status *status)
{
	pthread_mutex_destroy(&status->lock);
}

void
app_web_status_update(struct app_web_status *status, uint64_t records_in,
		       uint64_t records_dropped, uint64_t records_forwarded)
{
	pthread_mutex_lock(&status->lock);
	status->records_in = records_in;
	status->records_dropped = records_dropped;
	status->records_forwarded = records_forwarded;
	pthread_mutex_unlock(&status->lock);
}

void
app_web_status_update_stage_statuses(struct app_web_status *status,
				      const struct pipeline_chain *chain)
{
	/* Cheap enough (<= PIPELINE_MAX_STAGES stages, a few in-memory
	 * struct copies, no I/O - get_status() itself must not block, see
	 * stage.h's own comment) to just do the whole collection pass
	 * directly under the lock, same simple shape as
	 * app_web_status_update() above - no separate double-buffer needed
	 * for something this infrequent (--status-poll-interval, default
	 * 2s) and this cheap. */
	pthread_mutex_lock(&status->lock);
	status->stage_status_count = chain->stage_count;
	for (size_t i = 0; i < chain->stage_count; i++) {
		const struct pipeline_stage_instance *inst = &chain->stages[i];
		status->stage_statuses[i].node_id = inst->node_id;
		status->stage_statuses[i].stage_type = inst->stage->name;
		if (inst->stage->get_status != NULL)
			inst->stage->get_status(inst->state, &status->stage_statuses[i].status);
		else
			status->stage_statuses[i].status.field_count = 0;
	}
	pthread_mutex_unlock(&status->lock);
}

struct http_request {
	char method[8];
	char path[256];
	char *body;       /* malloc'd, NULL if none; caller frees */
	size_t body_len;
};

/* Case-insensitive search for a "Name: value" header within
 * [buf, header_end) - not a general header parser (this server only
 * ever needs Content-Length), same "just enough" scope as this file's
 * original single-purpose request-line reader. Returns a pointer to the
 * start of the value (leading spaces skipped), still followed by
 * whatever trailing "\r\n..." is in buf - fine here since strtoul()
 * (the only consumer) stops at the first non-digit on its own. */
static const char *
find_header_value_ci(const char *buf, const char *header_end, const char *name)
{
	size_t name_len = strlen(name);
	const char *line = buf;

	while (line < header_end) {
		const char *eol = memchr(line, '\n', (size_t)(header_end - line));
		if (eol == NULL)
			eol = header_end;
		if ((size_t)(eol - line) > name_len && strncasecmp(line, name, name_len) == 0 &&
		    line[name_len] == ':') {
			const char *v = line + name_len + 1;
			while (v < eol && (*v == ' ' || *v == '\r'))
				v++;
			return v;
		}
		line = eol + 1;
	}
	return NULL;
}

/* Splits path in place at its first '?' (if any), NUL-terminating the
 * path portion there and returning a pointer to whatever follows (the
 * query string, possibly empty, never NULL). Called once per request
 * before route dispatch, same "one path field" story req.path always
 * told before /api/graphs?name=... existed - a no-op for every route
 * that has no '?' in its request. */
static char *
split_query_string(char *path)
{
	char *q = strchr(path, '?');
	if (q == NULL)
		return path + strlen(path);
	*q = '\0';
	return q + 1;
}

static int
hex_digit_value(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

/* Extracts and percent-decodes the value of `key` from a query string
 * shaped like "k=v&k2=v2" into out (bounded by out_size). Just enough
 * for this server's own single "name" parameter - not a general query
 * parser, same minimal scope as find_header_value_ci() above. Returns
 * false if key isn't present or its decoded value doesn't fit. */
static bool
query_param(const char *query, const char *key, char *out, size_t out_size)
{
	size_t key_len = strlen(key);
	const char *p = query;

	while (*p != '\0') {
		const char *amp = strchr(p, '&');
		size_t pair_len = amp != NULL ? (size_t)(amp - p) : strlen(p);
		if (pair_len > key_len && p[key_len] == '=' && strncmp(p, key, key_len) == 0) {
			const char *v = p + key_len + 1;
			const char *v_end = p + pair_len;
			size_t out_off = 0;
			while (v < v_end) {
				if (out_off + 1 >= out_size)
					return false;
				if (*v == '+') {
					out[out_off++] = ' ';
					v++;
				} else if (*v == '%' && v + 2 < v_end) {
					int hi = hex_digit_value(v[1]);
					int lo = hex_digit_value(v[2]);
					if (hi < 0 || lo < 0)
						return false;
					out[out_off++] = (char)((hi << 4) | lo);
					v += 3;
				} else {
					out[out_off++] = *v++;
				}
			}
			out[out_off] = '\0';
			return true;
		}
		p = amp != NULL ? amp + 1 : p + pair_len;
	}
	return false;
}

/* A stored graph's name must be safe to append directly onto
 * graphs_dir to form a filesystem path - same defensive posture
 * serve_static_file() already takes with its own ".." rejection, plus
 * ruling out '/' outright (a name is a single path component, never a
 * sub-path) and requiring the ".json" suffix this server itself always
 * writes. */
static bool
valid_graph_name(const char *name)
{
	if (name == NULL || name[0] == '\0' || strlen(name) >= GRAPH_NAME_MAX_LEN)
		return false;
	if (strstr(name, "..") != NULL || strchr(name, '/') != NULL)
		return false;
	size_t len = strlen(name);
	return len > 5 && strcmp(name + len - 5, ".json") == 0;
}

/* Reads one request's method/path/headers and (if Content-Length says
 * so) body. Not a general HTTP parser - just enough for this server's
 * own fixed set of routes (see this file's header comment): no
 * chunked transfer-encoding, no pipelining, one request per connection
 * (matches send_response()'s existing "Connection: close" on every
 * reply). Returns false on a malformed/oversized request - caller
 * responds 400 and closes. */
static bool
read_http_request(int fd, struct http_request *req)
{
	memset(req, 0, sizeof(*req));

	char buf[REQUEST_HEADER_BUF_SIZE];
	size_t total = 0;
	char *header_end = NULL;

	while (total < sizeof(buf) - 1) {
		ssize_t n = recv(fd, buf + total, sizeof(buf) - 1 - total, 0);
		if (n <= 0)
			break;
		total += (size_t)n;
		buf[total] = '\0';
		header_end = strstr(buf, "\r\n\r\n");
		if (header_end != NULL)
			break;
	}
	if (header_end == NULL)
		return false;

	sscanf(buf, "%7s %255s", req->method, req->path);

	size_t content_length = 0;
	const char *cl = find_header_value_ci(buf, header_end, "Content-Length");
	if (cl != NULL)
		content_length = (size_t)strtoul(cl, NULL, 10);

	if (content_length == 0)
		return true;
	if (content_length > MAX_GRAPH_BODY_BYTES)
		return false;

	req->body = malloc(content_length + 1);
	if (req->body == NULL)
		return false;

	const char *body_start = header_end + 4;
	size_t already_have = (size_t)(buf + total - body_start);
	if (already_have > content_length)
		already_have = content_length;
	memcpy(req->body, body_start, already_have);

	size_t got = already_have;
	while (got < content_length) {
		ssize_t n = recv(fd, req->body + got, content_length - got, 0);
		if (n <= 0)
			break;
		got += (size_t)n;
	}
	req->body[got] = '\0';
	req->body_len = got;
	return got == content_length;
}

static void
send_response(int fd, const char *status_line, const char *content_type, const char *body)
{
	char header[256];
	int header_len = snprintf(header, sizeof(header),
		"HTTP/1.1 %s\r\nContent-Type: %s; charset=utf-8\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",
		status_line, content_type, strlen(body));
	if (header_len < 0)
		return;
	(void)send(fd, header, (size_t)header_len, MSG_NOSIGNAL);
	(void)send(fd, body, strlen(body), MSG_NOSIGNAL);
}

static void
handle_status_json(int fd, struct app_web_status *status)
{
	char json[JSON_BUF_SIZE];

	pthread_mutex_lock(&status->lock);
	snprintf(json, sizeof(json),
		"{\n"
		"  \"mode\": \"loomtabulator\",\n"
		"  \"uptime_sec\": %ld,\n"
		"  \"records_in\": %" PRIu64 ",\n"
		"  \"records_dropped\": %" PRIu64 ",\n"
		"  \"records_forwarded\": %" PRIu64 "\n"
		"}\n",
		(long)(time(NULL) - status->start_time),
		status->records_in, status->records_dropped, status->records_forwarded);
	pthread_mutex_unlock(&status->lock);

	send_response(fd, "200 OK", "application/json", json);
}

/* Serializes plugin_loader.c's dynamically-populated registry (every
 * successfully-loaded stage plugin, built-in or third-party) - the
 * Phase 3 web UI's palette and connection rules ("can I wire these two
 * ports together") derive from this response alone, never from a
 * client-side hardcoded list or a stage's internal behavior. */
static void
handle_stage_types(int fd)
{
	char json[STAGE_TYPES_JSON_BUF_SIZE];
	size_t count = stage_registry_count();
	int n = snprintf(json, sizeof(json), "[\n");
	size_t off = n > 0 ? (size_t)n : 0;

	for (size_t i = 0; i < count && off < sizeof(json); i++) {
		const struct stage *s = stage_registry_get(i);
		n = snprintf(json + off, sizeof(json) - off,
			"  { \"name\": \"%s\", \"in_types\": [", s->name);
		if (n > 0)
			off += (size_t)n;

		/* A stage's in_types is a bitmask (see stage.h) - one array
		 * element per set bit, iterating the enum's known sequential
		 * range same as graph_config.c's own describe_accepted_types(). */
		bool first_type = true;
		for (enum stage_port_type t = PORT_TYPE_RAW_RECORD; t <= PORT_TYPE_WIRE_FRAME && off < sizeof(json); t++) {
			if (!(s->in_types & PORT_TYPE_BIT(t)))
				continue;
			n = snprintf(json + off, sizeof(json) - off, "%s\"%s\"",
				      first_type ? "" : ", ", stage_port_type_name(t));
			if (n > 0)
				off += (size_t)n;
			first_type = false;
		}

		n = snprintf(json + off, sizeof(json) - off, "], \"out_type\": \"%s\" }%s\n",
			stage_port_type_name(s->out_type), i + 1 < count ? "," : "");
		if (n > 0)
			off += (size_t)n;
	}
	if (off < sizeof(json)) {
		n = snprintf(json + off, sizeof(json) - off, "]\n");
		if (n > 0)
			off += (size_t)n;
	}

	send_response(fd, "200 OK", "application/json", json);
}

/* GET /api/graph: the last graph that was successfully saved (either
 * --graph=PATH at startup or the most recent successful POST) - served
 * verbatim as the raw text loaded/POSTed, not reconstructed from the
 * live pipeline_chain (which has already discarded node ids/positions/
 * original config values by the time graph_config_load() finishes - see
 * graph_config.c). This is what lets the web UI open showing the graph
 * that's actually running instead of a blank canvas. */
static void
handle_get_graph(int fd, const struct web_graph_ctx *graph_ctx)
{
	if (graph_ctx == NULL || graph_ctx->current_graph_json == NULL) {
		send_response(fd, "501 Not Implemented", "text/plain", "graph API not enabled\n");
		return;
	}
	send_response(fd, "200 OK", "application/json", graph_ctx->current_graph_json);
}

/* Appends src into dst (bounded by dst_size), escaping '"' and '\\' so
 * the result is safe to embed inside a JSON string literal -
 * graph_config.c's error messages can echo back attacker-controlled
 * node ids/types straight out of the POSTed graph (e.g. "unknown stage
 * type '%s'"), so this is correctness (valid JSON out), not just
 * cosmetic. */
static void
json_escape_append(char *dst, size_t dst_size, size_t *off, const char *src)
{
	for (; *src != '\0' && *off + 2 < dst_size; src++) {
		if (*src == '"' || *src == '\\')
			dst[(*off)++] = '\\';
		dst[(*off)++] = *src;
	}
	dst[*off] = '\0';
}

/* GET /api/stage-status: struct app_web_status.stage_statuses[], filled
 * periodically by app_web_status_update_stage_statuses() (called from
 * main.c's own slow --status-poll-interval loop, never per-record) -
 * see stage.h's struct stage.get_status comment for the full mechanism.
 * node_id/stage_type/field names are escaped (json_escape_append()
 * above) since node_id and field names both ultimately come from
 * user-authored content (the graph JSON's own node ids, and whatever a
 * third-party plugin's get_status() reports) - same defensive posture
 * handle_post_graph() already takes with graph_config.c's own error
 * strings. */
static void
handle_stage_status(int fd, struct app_web_status *status)
{
	char json[STAGE_TYPES_JSON_BUF_SIZE];
	size_t off = 0;

	pthread_mutex_lock(&status->lock);
	int n = snprintf(json, sizeof(json), "[\n");
	if (n > 0)
		off = (size_t)n;

	for (size_t i = 0; i < status->stage_status_count && off + 2 < sizeof(json); i++) {
		const struct stage_status_snapshot *snap = &status->stage_statuses[i];

		n = snprintf(json + off, sizeof(json) - off, "  { \"node_id\": \"");
		if (n > 0)
			off += (size_t)n;
		json_escape_append(json, sizeof(json), &off, snap->node_id);
		n = snprintf(json + off, sizeof(json) - off, "\", \"type\": \"");
		if (n > 0)
			off += (size_t)n;
		json_escape_append(json, sizeof(json), &off, snap->stage_type);
		n = snprintf(json + off, sizeof(json) - off, "\", \"fields\": [");
		if (n > 0)
			off += (size_t)n;

		for (unsigned f = 0; f < snap->status.field_count && off + 2 < sizeof(json); f++) {
			n = snprintf(json + off, sizeof(json) - off, "%s{ \"name\": \"",
				      f > 0 ? ", " : "");
			if (n > 0)
				off += (size_t)n;
			json_escape_append(json, sizeof(json), &off, snap->status.fields[f].name);
			n = snprintf(json + off, sizeof(json) - off, "\", \"value\": %" PRIu64 " }",
				      snap->status.fields[f].value);
			if (n > 0)
				off += (size_t)n;
		}

		n = snprintf(json + off, sizeof(json) - off, "] }%s\n",
			      i + 1 < status->stage_status_count ? "," : "");
		if (n > 0)
			off += (size_t)n;
	}
	pthread_mutex_unlock(&status->lock);

	if (off < sizeof(json)) {
		n = snprintf(json + off, sizeof(json) - off, "]\n");
		if (n > 0)
			off += (size_t)n;
	}

	send_response(fd, "200 OK", "application/json", json);
}

/* Validates a graph file at `path` exactly as startup does - same
 * errors, same rules (a linear chain, known stage types, matching port
 * types) - via graph_config_load(), then immediately tears down the
 * chain it built. The chain only ever exists here to prove the graph
 * is valid; it's never used to run anything. Shared by handle_post_graph()
 * (path is a staged temp file holding a POST body) and
 * handle_post_load_graph() (path is a file already sitting in
 * graphs_dir) - same validation either way. */
static bool
validate_graph_file(const char *path, char *errbuf, size_t errbuf_len)
{
	struct pipeline_chain throwaway;
	struct graph_config_result info;
	bool ok = graph_config_load(path, &throwaway, &info, errbuf, errbuf_len);
	if (ok)
		for (size_t i = 0; i < throwaway.stage_count; i++)
			if (throwaway.stages[i].stage->teardown != NULL)
				throwaway.stages[i].stage->teardown(throwaway.stages[i].state);
	return ok;
}

/* Writes content to graph_ctx->graph_path (the file --graph=PATH
 * loaded at startup) and swaps in a fresh current_graph_json so
 * GET /api/graph immediately reflects it. Shared by handle_post_graph()
 * (content is the just-validated POST body) and handle_post_load_graph()
 * (content is a just-validated library graph's own bytes) - "make this
 * the active graph" means exactly the same thing either way. Does NOT
 * touch the running pipeline - see this file's header comment for why
 * hot-swapping wasn't worth the added risk to epoch_barrier.c's
 * worker-pool synchronization; the caller's response says a restart is
 * needed, and the web UI surfaces that rather than claiming it's live. */
static bool
activate_graph(struct web_graph_ctx *graph_ctx, const char *content, size_t content_len)
{
	FILE *out = fopen(graph_ctx->graph_path, "wb");
	size_t written = out != NULL ? fwrite(content, 1, content_len, out) : 0;
	if (out != NULL)
		fclose(out);
	if (out == NULL || written != content_len)
		return false;

	char *copy = malloc(content_len + 1);
	if (copy != NULL) {
		memcpy(copy, content, content_len);
		copy[content_len] = '\0';
		free(graph_ctx->current_graph_json);
		graph_ctx->current_graph_json = copy;
		graph_ctx->current_graph_len = content_len;
	}
	return true;
}

/* Builds a "{ \"ok\": false, \"error\": \"...\" }" response body from a
 * graph_config_load()-style errbuf and sends it as 400 - shared by
 * every handler that surfaces a validation failure the same way. errbuf
 * is at most 256 bytes (graph_config_load()'s own errbuf_len) but
 * json_escape_append() can double it in the pathological case of an
 * error string that's all quotes/backslashes - sized generously above
 * that worst case rather than relying on the truncation guard below. */
static void
send_validation_error(int fd, const char *errbuf)
{
	char json[768];
	size_t off = (size_t)snprintf(json, sizeof(json), "{ \"ok\": false, \"error\": \"");
	json_escape_append(json, sizeof(json), &off, errbuf);
	if (off + 4 < sizeof(json))
		off += (size_t)snprintf(json + off, sizeof(json) - off, "\" }\n");
	send_response(fd, "400 Bad Request", "application/json", json);
}

/* POST /api/graph: validates the uploaded graph and, on success, saves
 * it to --graph=PATH's file (see activate_graph() above). Writes the
 * POST body to a temp file because graph_config_load() (and
 * json_parse() underneath it) takes a file path, same as it does at
 * startup - reusing that exact validation path, rather than a second
 * one for in-memory bodies, is the whole point (same errors either
 * way). */
static void
handle_post_graph(int fd, const char *body, size_t body_len, struct web_graph_ctx *graph_ctx)
{
	if (graph_ctx == NULL) {
		send_response(fd, "501 Not Implemented", "text/plain", "graph API not enabled\n");
		return;
	}

	char tmp_path[] = "/tmp/loomtabulator-graph-XXXXXX";
	int tmp_fd = mkstemp(tmp_path);
	if (tmp_fd < 0) {
		send_response(fd, "500 Internal Server Error", "text/plain", "mkstemp failed\n");
		return;
	}
	ssize_t written = write(tmp_fd, body, body_len);
	close(tmp_fd);
	if (written < 0 || (size_t)written != body_len) {
		unlink(tmp_path);
		send_response(fd, "500 Internal Server Error", "text/plain", "failed to stage uploaded graph\n");
		return;
	}

	char errbuf[256];
	bool ok = validate_graph_file(tmp_path, errbuf, sizeof(errbuf));
	unlink(tmp_path);

	if (!ok) {
		send_validation_error(fd, errbuf);
		return;
	}

	if (!activate_graph(graph_ctx, body, body_len)) {
		send_response(fd, "500 Internal Server Error", "text/plain", "failed to save graph file\n");
		return;
	}

	send_response(fd, "200 OK", "application/json",
		      "{ \"ok\": true, \"restart_required\": true }\n");
}

/* Reads path's raw bytes into a malloc'd, NUL-terminated buffer - same
 * shape as main.c's own read_file_contents(), duplicated here rather
 * than shared across modules for something this small. Used only by
 * handle_post_load_graph() below to pull a library graph's bytes into
 * memory before handing them to activate_graph(). */
static char *
read_whole_file_here(const char *path, size_t *out_len)
{
	FILE *f = fopen(path, "rb");
	if (f == NULL)
		return NULL;
	fseek(f, 0, SEEK_END);
	long size = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (size < 0) {
		fclose(f);
		return NULL;
	}
	char *buf = malloc((size_t)size + 1);
	if (buf == NULL) {
		fclose(f);
		return NULL;
	}
	size_t n = fread(buf, 1, (size_t)size, f);
	fclose(f);
	buf[n] = '\0';
	if (out_len != NULL)
		*out_len = n;
	return buf;
}

/* GET /api/graphs: lists the named graphs sitting in --graphs-dir (see
 * struct web_graph_ctx.graphs_dir) - the library POST/DELETE below add
 * to and remove from. Sorted by name for a stable UI listing. Distinct
 * from GET /api/graph (singular) above, which serves the one currently
 * *active* graph's text, not this directory's contents. */
static void
handle_get_graphs(int fd, const struct web_graph_ctx *graph_ctx)
{
	if (graph_ctx == NULL || graph_ctx->graphs_dir == NULL) {
		send_response(fd, "501 Not Implemented", "text/plain", "graph library not enabled\n");
		return;
	}

	DIR *d = opendir(graph_ctx->graphs_dir);
	if (d == NULL) {
		send_response(fd, "500 Internal Server Error", "text/plain", "failed to open graphs directory\n");
		return;
	}

	char names[MAX_GRAPHS_LISTED][GRAPH_NAME_MAX_LEN];
	time_t mtimes[MAX_GRAPHS_LISTED];
	size_t count = 0;

	struct dirent *entry;
	while (count < MAX_GRAPHS_LISTED && (entry = readdir(d)) != NULL) {
		if (!valid_graph_name(entry->d_name))
			continue;

		char full_path[GRAPHS_DIR_MAX_PATH];
		if (snprintf(full_path, sizeof(full_path), "%s/%s", graph_ctx->graphs_dir,
			     entry->d_name) >= (int)sizeof(full_path))
			continue;

		struct stat st;
		if (stat(full_path, &st) != 0 || !S_ISREG(st.st_mode))
			continue;

		snprintf(names[count], sizeof(names[count]), "%s", entry->d_name);
		mtimes[count] = st.st_mtime;
		count++;
	}
	closedir(d);

	/* Simple insertion sort by name - count is capped at
	 * MAX_GRAPHS_LISTED (a directory of stored graphs, not a hot
	 * path), so O(n^2) here is not worth pulling in qsort()'s
	 * comparator-and-cast ceremony for. */
	for (size_t i = 1; i < count; i++) {
		char tmp_name[GRAPH_NAME_MAX_LEN];
		time_t tmp_mtime = mtimes[i];
		snprintf(tmp_name, sizeof(tmp_name), "%s", names[i]);
		size_t j = i;
		while (j > 0 && strcmp(names[j - 1], tmp_name) > 0) {
			snprintf(names[j], sizeof(names[j]), "%s", names[j - 1]);
			mtimes[j] = mtimes[j - 1];
			j--;
		}
		snprintf(names[j], sizeof(names[j]), "%s", tmp_name);
		mtimes[j] = tmp_mtime;
	}

	char json[GRAPHS_LIST_JSON_BUF_SIZE];
	size_t off = (size_t)snprintf(json, sizeof(json), "[\n");
	for (size_t i = 0; i < count && off + 2 < sizeof(json); i++) {
		int n = snprintf(json + off, sizeof(json) - off,
			"  { \"name\": \"%s\", \"mtime\": %" PRId64 " }%s\n",
			names[i], (int64_t)mtimes[i], i + 1 < count ? "," : "");
		if (n > 0)
			off += (size_t)n;
	}
	if (off + 2 < sizeof(json))
		off += (size_t)snprintf(json + off, sizeof(json) - off, "]\n");

	send_response(fd, "200 OK", "application/json", json);
}

/* POST /api/graphs?name=X: validates the POST body exactly as
 * POST /api/graph does (same graph_config_load() rules) and, on
 * success, writes it into --graphs-dir/X (creating or overwriting) -
 * unlike POST /api/graph, this never touches the *active* graph, so no
 * restart is ever implied by a successful upload. */
static void
handle_post_upload_graph(int fd, const char *query, const char *body, size_t body_len,
			  const struct web_graph_ctx *graph_ctx)
{
	if (graph_ctx == NULL || graph_ctx->graphs_dir == NULL) {
		send_response(fd, "501 Not Implemented", "text/plain", "graph library not enabled\n");
		return;
	}

	char name[GRAPH_NAME_MAX_LEN];
	if (!query_param(query, "name", name, sizeof(name)) || !valid_graph_name(name)) {
		send_response(fd, "400 Bad Request", "application/json",
			      "{ \"ok\": false, \"error\": \"missing or invalid 'name' "
			      "(must be a bare filename ending in .json)\" }\n");
		return;
	}

	char tmp_path[] = "/tmp/loomtabulator-graph-XXXXXX";
	int tmp_fd = mkstemp(tmp_path);
	if (tmp_fd < 0) {
		send_response(fd, "500 Internal Server Error", "text/plain", "mkstemp failed\n");
		return;
	}
	ssize_t written = write(tmp_fd, body, body_len);
	close(tmp_fd);
	if (written < 0 || (size_t)written != body_len) {
		unlink(tmp_path);
		send_response(fd, "500 Internal Server Error", "text/plain", "failed to stage uploaded graph\n");
		return;
	}

	char errbuf[256];
	bool ok = validate_graph_file(tmp_path, errbuf, sizeof(errbuf));
	unlink(tmp_path);

	if (!ok) {
		send_validation_error(fd, errbuf);
		return;
	}

	char full_path[GRAPHS_DIR_MAX_PATH];
	if (snprintf(full_path, sizeof(full_path), "%s/%s", graph_ctx->graphs_dir, name) >=
	    (int)sizeof(full_path)) {
		send_response(fd, "500 Internal Server Error", "text/plain", "graphs_dir path too long\n");
		return;
	}

	FILE *out = fopen(full_path, "wb");
	size_t out_written = out != NULL ? fwrite(body, 1, body_len, out) : 0;
	if (out != NULL)
		fclose(out);
	if (out == NULL || out_written != body_len) {
		send_response(fd, "500 Internal Server Error", "text/plain", "failed to write graph file\n");
		return;
	}

	send_response(fd, "200 OK", "application/json", "{ \"ok\": true }\n");
}

/* POST /api/graphs/load?name=X: (re-)validates --graphs-dir/X (already
 * a real file, no temp-staging needed, unlike the upload/POST /api/graph
 * paths above) and, on success, activates it exactly as POST /api/graph
 * does (see activate_graph()) - same "restart_required: true" response
 * shape. Re-validating here (rather than trusting the earlier upload-time
 * check) covers a file that was dropped into graphs_dir by hand outside
 * the API, or has since gone stale for some other reason. */
static void
handle_post_load_graph(int fd, const char *query, struct web_graph_ctx *graph_ctx)
{
	if (graph_ctx == NULL || graph_ctx->graphs_dir == NULL) {
		send_response(fd, "501 Not Implemented", "text/plain", "graph library not enabled\n");
		return;
	}

	char name[GRAPH_NAME_MAX_LEN];
	if (!query_param(query, "name", name, sizeof(name)) || !valid_graph_name(name)) {
		send_response(fd, "400 Bad Request", "application/json",
			      "{ \"ok\": false, \"error\": \"missing or invalid 'name'\" }\n");
		return;
	}

	char full_path[GRAPHS_DIR_MAX_PATH];
	if (snprintf(full_path, sizeof(full_path), "%s/%s", graph_ctx->graphs_dir, name) >=
	    (int)sizeof(full_path)) {
		send_response(fd, "500 Internal Server Error", "text/plain", "graphs_dir path too long\n");
		return;
	}

	char errbuf[256];
	if (!validate_graph_file(full_path, errbuf, sizeof(errbuf))) {
		send_validation_error(fd, errbuf);
		return;
	}

	size_t content_len;
	char *content = read_whole_file_here(full_path, &content_len);
	if (content == NULL) {
		send_response(fd, "500 Internal Server Error", "text/plain", "failed to read graph file\n");
		return;
	}

	bool activated = activate_graph(graph_ctx, content, content_len);
	free(content);
	if (!activated) {
		send_response(fd, "500 Internal Server Error", "text/plain", "failed to activate graph\n");
		return;
	}

	send_response(fd, "200 OK", "application/json",
		      "{ \"ok\": true, \"restart_required\": true }\n");
}

/* DELETE /api/graphs?name=X: removes X from --graphs-dir. Never touches
 * the active graph_path/current_graph_json - loading copies bytes
 * (activate_graph() above), it doesn't symlink, so the two are always
 * independent files; deleting a library entry that happens to also be
 * the one currently active is not a special case. */
static void
handle_delete_graph(int fd, const char *query, const struct web_graph_ctx *graph_ctx)
{
	if (graph_ctx == NULL || graph_ctx->graphs_dir == NULL) {
		send_response(fd, "501 Not Implemented", "text/plain", "graph library not enabled\n");
		return;
	}

	char name[GRAPH_NAME_MAX_LEN];
	if (!query_param(query, "name", name, sizeof(name)) || !valid_graph_name(name)) {
		send_response(fd, "400 Bad Request", "application/json",
			      "{ \"ok\": false, \"error\": \"missing or invalid 'name'\" }\n");
		return;
	}

	char full_path[GRAPHS_DIR_MAX_PATH];
	if (snprintf(full_path, sizeof(full_path), "%s/%s", graph_ctx->graphs_dir, name) >=
	    (int)sizeof(full_path)) {
		send_response(fd, "500 Internal Server Error", "text/plain", "graphs_dir path too long\n");
		return;
	}

	if (unlink(full_path) != 0) {
		send_response(fd, errno == ENOENT ? "404 Not Found" : "500 Internal Server Error",
			      "application/json", "{ \"ok\": false, \"error\": \"no such graph\" }\n");
		return;
	}

	send_response(fd, "200 OK", "application/json", "{ \"ok\": true }\n");
}

/* POST /api/reload: triggers a full, graceful restart, applying
 * whatever graph is currently saved at --graph=PATH (normally just
 * POSTed via handle_post_graph() above, though nothing requires that
 * order - a reload with no prior save just re-loads the same file
 * unchanged). Sends the response FIRST, then sets the flags - by the
 * time the client sees this response, the actual shutdown+re-exec
 * hasn't necessarily started yet, but it's already committed to
 * happening (this connection's own fd is independent of the listening
 * socket main.c's shutdown sequence will close, so finishing this
 * response is unaffected either way). See main.c's own
 * g_reload_requested comment for what actually happens after these
 * flags are set - this function's only job is setting them. */
static void
handle_post_reload(int fd, volatile bool *quit_flag, volatile bool *reload_flag)
{
	send_response(fd, "200 OK", "application/json", "{ \"ok\": true, \"reloading\": true }\n");
	*reload_flag = true;
	*quit_flag = true;
}

/* POST /api/probe-port-count: the web UI needs to know how many output
 * ports a node will have, to decide how many handles to draw on it -
 * but out_port_count() is a per-INSTANCE, config-dependent property
 * (see stage.h), not a fixed property of a stage TYPE the way
 * in_type/out_type are, so GET /api/stage-types can't answer this (it
 * lists types, not instances), and there's still no per-stage config
 * editor for the client to simulate the answer itself. This builds a
 * real, throwaway instance - init() + out_port_count() + teardown(),
 * immediately - exactly the same validate-then-discard shape
 * handle_post_graph() above already uses, just for one node's
 * {"type", "config"} instead of a whole graph, and with no temp file
 * needed (json_parse() works directly on an in-memory buffer; only
 * graph_config_load() needs a path). Never touches the real graph or
 * the running pipeline. */
static void
handle_probe_port_count(int fd, const char *body, size_t body_len)
{
	/* json_parse() mutates its input in place and requires it to
	 * outlive the returned tree - a private, heap-owned copy, not the
	 * caller's buffer (which may be the "" literal handle_connection
	 * passes for a bodyless request). */
	char *buf = malloc(body_len + 1);
	if (buf == NULL) {
		send_response(fd, "500 Internal Server Error", "application/json",
			      "{ \"error\": \"out of memory\" }\n");
		return;
	}
	memcpy(buf, body, body_len);
	buf[body_len] = '\0';

	char parse_errbuf[128];
	struct json_value *root = json_parse(buf, parse_errbuf, sizeof(parse_errbuf));
	if (root == NULL) {
		char json[256];
		size_t off = (size_t)snprintf(json, sizeof(json), "{ \"error\": \"");
		json_escape_append(json, sizeof(json), &off, parse_errbuf);
		if (off + 4 < sizeof(json))
			off += (size_t)snprintf(json + off, sizeof(json) - off, "\" }\n");
		send_response(fd, "400 Bad Request", "application/json", json);
		free(buf);
		return;
	}

	const char *type = json_as_string(json_object_get(root, "type"), NULL);
	const struct stage *stage = type != NULL ? stage_registry_find(type) : NULL;
	if (stage == NULL) {
		send_response(fd, "400 Bad Request", "application/json",
			      "{ \"error\": \"unknown stage type\" }\n");
		json_free(root);
		free(buf);
		return;
	}

	void *state = stage->init(json_object_get(root, "config"));
	if (state == NULL) {
		send_response(fd, "400 Bad Request", "application/json",
			      "{ \"error\": \"failed to initialize this stage - check its config\" }\n");
		json_free(root);
		free(buf);
		return;
	}

	unsigned port_count = stage->out_port_count != NULL ? stage->out_port_count(state) : 1;
	if (stage->teardown != NULL)
		stage->teardown(state);
	json_free(root);
	free(buf);

	char json[64];
	snprintf(json, sizeof(json), "{ \"port_count\": %u }\n", port_count);
	send_response(fd, "200 OK", "application/json", json);
}

static const char *
content_type_for_path(const char *path)
{
	const char *dot = strrchr(path, '.');
	if (dot == NULL)
		return "application/octet-stream";
	if (strcmp(dot, ".html") == 0) return "text/html";
	if (strcmp(dot, ".js") == 0) return "application/javascript";
	if (strcmp(dot, ".css") == 0) return "text/css";
	if (strcmp(dot, ".json") == 0) return "application/json";
	if (strcmp(dot, ".svg") == 0) return "image/svg+xml";
	if (strcmp(dot, ".ico") == 0) return "image/x-icon";
	if (strcmp(dot, ".png") == 0) return "image/png";
	if (strcmp(dot, ".woff2") == 0) return "font/woff2";
	return "application/octet-stream";
}

static void
send_file_response(int fd, const char *content_type, const void *body, size_t body_len)
{
	char header[256];
	int header_len = snprintf(header, sizeof(header),
		"HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",
		content_type, body_len);
	if (header_len < 0)
		return;
	(void)send(fd, header, (size_t)header_len, MSG_NOSIGNAL);
	(void)send(fd, body, body_len, MSG_NOSIGNAL);
}

/* Serves the Phase 3 web UI's built web/dist/ as plain static files -
 * no templating, no embedding as one big C string like this project
 * family's own status pages, because Vite's output is multi-file and
 * content-hashed (see CLAUDE.md's Phase 3 design sketch). Rejects any
 * path containing ".." outright rather than resolving and re-checking
 * against web_root - simplest correct rule for a server with no auth
 * in front of it. */
static void
serve_static_file(int fd, const char *web_root, const char *req_path)
{
	if (web_root == NULL || web_root[0] == '\0' || strstr(req_path, "..") != NULL) {
		send_response(fd, "404 Not Found", "text/plain", "not found\n");
		return;
	}

	const char *rel_path = strcmp(req_path, "/") == 0 ? "/index.html" : req_path;

	char full_path[WEB_ROOT_MAX_PATH];
	if (snprintf(full_path, sizeof(full_path), "%s%s", web_root, rel_path) >= (int)sizeof(full_path)) {
		send_response(fd, "404 Not Found", "text/plain", "not found\n");
		return;
	}

	FILE *f = fopen(full_path, "rb");
	if (f == NULL) {
		send_response(fd, "404 Not Found", "text/plain", "not found\n");
		return;
	}

	fseek(f, 0, SEEK_END);
	long size = ftell(f);
	if (size < 0 || size > STATIC_FILE_MAX_BYTES) {
		fclose(f);
		send_response(fd, "404 Not Found", "text/plain", "not found\n");
		return;
	}
	fseek(f, 0, SEEK_SET);

	void *buf = malloc((size_t)size);
	if (buf == NULL || fread(buf, 1, (size_t)size, f) != (size_t)size) {
		free(buf);
		fclose(f);
		send_response(fd, "500 Internal Server Error", "text/plain", "read error\n");
		return;
	}
	fclose(f);

	send_file_response(fd, content_type_for_path(full_path), buf, (size_t)size);
	free(buf);
}

static void
handle_connection(int fd, struct app_web_status *status, const char *web_root,
		   struct web_graph_ctx *graph_ctx, volatile bool *quit_flag,
		   volatile bool *reload_flag)
{
	struct http_request req;
	if (!read_http_request(fd, &req)) {
		send_response(fd, "400 Bad Request", "text/plain", "bad request\n");
		free(req.body);
		return;
	}

	char *query = split_query_string(req.path);

	if (strcmp(req.method, "GET") == 0 && strcmp(req.path, "/status.json") == 0) {
		handle_status_json(fd, status);
	} else if (strcmp(req.method, "GET") == 0 && strcmp(req.path, "/api/stage-types") == 0) {
		handle_stage_types(fd);
	} else if (strcmp(req.method, "GET") == 0 && strcmp(req.path, "/api/graph") == 0) {
		handle_get_graph(fd, graph_ctx);
	} else if (strcmp(req.method, "GET") == 0 && strcmp(req.path, "/api/stage-status") == 0) {
		handle_stage_status(fd, status);
	} else if (strcmp(req.method, "POST") == 0 && strcmp(req.path, "/api/graph") == 0) {
		handle_post_graph(fd, req.body != NULL ? req.body : "", req.body_len, graph_ctx);
	} else if (strcmp(req.method, "POST") == 0 && strcmp(req.path, "/api/reload") == 0) {
		handle_post_reload(fd, quit_flag, reload_flag);
	} else if (strcmp(req.method, "POST") == 0 && strcmp(req.path, "/api/probe-port-count") == 0) {
		handle_probe_port_count(fd, req.body != NULL ? req.body : "", req.body_len);
	} else if (strcmp(req.method, "GET") == 0 && strcmp(req.path, "/api/graphs") == 0) {
		handle_get_graphs(fd, graph_ctx);
	} else if (strcmp(req.method, "POST") == 0 && strcmp(req.path, "/api/graphs") == 0) {
		handle_post_upload_graph(fd, query, req.body != NULL ? req.body : "", req.body_len, graph_ctx);
	} else if (strcmp(req.method, "POST") == 0 && strcmp(req.path, "/api/graphs/load") == 0) {
		handle_post_load_graph(fd, query, graph_ctx);
	} else if (strcmp(req.method, "DELETE") == 0 && strcmp(req.path, "/api/graphs") == 0) {
		handle_delete_graph(fd, query, graph_ctx);
	} else if (strcmp(req.method, "GET") == 0) {
		serve_static_file(fd, web_root, req.path);
	} else {
		send_response(fd, "404 Not Found", "text/plain", "not found\n");
	}

	free(req.body);
}

static void *
server_thread_main(void *arg)
{
	struct web_server_ctx *ctx = arg;

	while (!*ctx->quit_flag) {
		struct pollfd pfd = { .fd = ctx->listen_fd, .events = POLLIN };
		int ret = poll(&pfd, 1, ACCEPT_POLL_TIMEOUT_MS);
		if (ret <= 0)
			continue;

		int conn_fd = accept(ctx->listen_fd, NULL, NULL);
		if (conn_fd < 0)
			continue;

		handle_connection(conn_fd, ctx->status, ctx->web_root, ctx->graph_ctx,
				   ctx->quit_flag, ctx->reload_flag);
		close(conn_fd);
	}
	return NULL;
}

int
web_status_start(uint16_t web_port, struct app_web_status *status, volatile bool *quit_flag,
		  volatile bool *reload_flag, const char *web_root, struct web_graph_ctx *graph_ctx)
{
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) {
		fprintf(stderr, "web_status: socket() failed: %s\n", strerror(errno));
		return -1;
	}

	int one = 1;
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

	struct sockaddr_in addr = {
		.sin_family = AF_INET,
		.sin_addr.s_addr = INADDR_ANY,
		.sin_port = htons(web_port),
	};

	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
		fprintf(stderr, "web_status: bind() to port %u failed: %s\n", web_port, strerror(errno));
		close(fd);
		return -1;
	}
	if (listen(fd, 8) != 0) {
		fprintf(stderr, "web_status: listen() failed: %s\n", strerror(errno));
		close(fd);
		return -1;
	}

	g_ctx.listen_fd = fd;
	g_ctx.status = status;
	g_ctx.quit_flag = quit_flag;
	g_ctx.reload_flag = reload_flag;
	g_ctx.web_root = web_root;
	g_ctx.graph_ctx = graph_ctx;

	int ret = pthread_create(&g_server_thread, NULL, server_thread_main, &g_ctx);
	if (ret != 0) {
		fprintf(stderr, "web_status: pthread_create failed: %s\n", strerror(ret));
		close(fd);
		return -1;
	}

	g_server_running = true;
	return 0;
}

void
web_status_stop(void)
{
	if (!g_server_running)
		return;
	pthread_join(g_server_thread, NULL);
	close(g_ctx.listen_fd);
	g_server_running = false;
}
