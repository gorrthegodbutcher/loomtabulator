#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include "graph_config.h"
#include "plugin_loader.h"
#include "json.h"

#define MAX_NODES PIPELINE_MAX_STAGES

struct node_info {
	const char *id;
	const char *type;
	const struct json_value *config; /* node.data.config, may be NULL/empty */
	int in_degree;
	int out_degree;
	int edge_target; /* index into nodes[], -1 if out_degree == 0 */
};

static char *
read_whole_file(const char *path, char *errbuf, size_t errbuf_len)
{
	FILE *f = fopen(path, "rb");
	if (f == NULL) {
		snprintf(errbuf, errbuf_len, "cannot open '%s': %s", path, strerror(errno));
		return NULL;
	}
	fseek(f, 0, SEEK_END);
	long size = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (size < 0) {
		fclose(f);
		snprintf(errbuf, errbuf_len, "cannot determine size of '%s'", path);
		return NULL;
	}

	/* Intentionally never freed - this is loaded once at startup and the
	 * parsed json_value tree's strings point directly into it for the
	 * process's entire lifetime, same "one-time alloc, no need to ever
	 * free it" shape as e.g. chrontabulator's own vol_buf. */
	char *buf = malloc((size_t)size + 1);
	if (buf == NULL) {
		fclose(f);
		snprintf(errbuf, errbuf_len, "out of memory reading '%s'", path);
		return NULL;
	}
	size_t n = fread(buf, 1, (size_t)size, f);
	fclose(f);
	buf[n] = '\0';
	return buf;
}

static bool
find_node_idx(struct node_info *nodes, size_t count, const char *id, size_t *out_idx)
{
	for (size_t i = 0; i < count; i++) {
		if (strcmp(nodes[i].id, id) == 0) {
			*out_idx = i;
			return true;
		}
	}
	return false;
}

bool
graph_config_load(const char *path, struct pipeline_chain *chain, struct graph_config_result *out,
		   char *errbuf, size_t errbuf_len)
{
	memset(chain, 0, sizeof(*chain));

	char *text = read_whole_file(path, errbuf, errbuf_len);
	if (text == NULL)
		return false;

	struct json_value *root = json_parse(text, errbuf, errbuf_len);
	if (root == NULL)
		return false;

	const struct json_value *input = json_object_get(root, "input");
	const char *ring_name = json_as_string(json_object_get(input, "ring_name"), NULL);
	if (ring_name == NULL || strlen(ring_name) >= sizeof(out->ring_name)) {
		snprintf(errbuf, errbuf_len, "\"input.ring_name\" is required and must be short");
		return false;
	}
	snprintf(out->ring_name, sizeof(out->ring_name), "%s", ring_name);
	out->ring_size = (unsigned int)json_as_number(json_object_get(input, "ring_size"), 4096);

	const struct json_value *nodes_json = json_object_get(root, "nodes");
	const struct json_value *edges_json = json_object_get(root, "edges");
	size_t node_count = json_array_size(nodes_json);
	size_t edge_count = json_array_size(edges_json);

	if (node_count == 0) {
		snprintf(errbuf, errbuf_len, "graph has no nodes");
		return false;
	}
	if (node_count > MAX_NODES) {
		snprintf(errbuf, errbuf_len, "graph has %zu nodes, max is %d", node_count, MAX_NODES);
		return false;
	}

	struct node_info nodes[MAX_NODES] = {0};
	for (size_t i = 0; i < node_count; i++) {
		const struct json_value *n = json_array_get(nodes_json, i);
		nodes[i].id = json_as_string(json_object_get(n, "id"), NULL);
		nodes[i].type = json_as_string(json_object_get(n, "type"), NULL);
		const struct json_value *data = json_object_get(n, "data");
		nodes[i].config = json_object_get(data, "config");
		nodes[i].edge_target = -1;
		if (nodes[i].id == NULL || nodes[i].type == NULL) {
			snprintf(errbuf, errbuf_len, "node %zu is missing \"id\" or \"type\"", i);
			return false;
		}
	}

	for (size_t e = 0; e < edge_count; e++) {
		const struct json_value *edge = json_array_get(edges_json, e);
		const char *src = json_as_string(json_object_get(edge, "source"), NULL);
		const char *dst = json_as_string(json_object_get(edge, "target"), NULL);
		size_t src_idx, dst_idx;
		if (src == NULL || dst == NULL ||
		    !find_node_idx(nodes, node_count, src, &src_idx) ||
		    !find_node_idx(nodes, node_count, dst, &dst_idx)) {
			snprintf(errbuf, errbuf_len, "edge %zu references an unknown node id", e);
			return false;
		}
		nodes[src_idx].out_degree++;
		nodes[src_idx].edge_target = (int)dst_idx;
		nodes[dst_idx].in_degree++;
	}

	/* v1 requires a single linear chain - see graph_config.h's header
	 * comment for why the schema itself doesn't enforce this (so Phase
	 * 2/3 branching graphs don't need a version bump). */
	size_t start_idx = SIZE_MAX;
	for (size_t i = 0; i < node_count; i++) {
		if (nodes[i].in_degree > 1 || nodes[i].out_degree > 1) {
			snprintf(errbuf, errbuf_len,
				 "node '%s' has more than one incoming or outgoing edge - "
				 "v1 only supports a single linear chain", nodes[i].id);
			return false;
		}
		if (nodes[i].in_degree == 0) {
			if (start_idx != SIZE_MAX) {
				snprintf(errbuf, errbuf_len,
					 "graph has more than one starting node ('%s' and '%s') - "
					 "v1 only supports a single linear chain",
					 nodes[start_idx].id, nodes[i].id);
				return false;
			}
			start_idx = i;
		}
	}
	if (start_idx == SIZE_MAX) {
		snprintf(errbuf, errbuf_len, "graph has no starting node (every node has an incoming edge - a cycle?)");
		return false;
	}

	size_t order[MAX_NODES];
	size_t order_count = 0;
	size_t cur = start_idx;
	for (;;) {
		order[order_count++] = cur;
		if (nodes[cur].edge_target < 0)
			break;
		if (order_count >= node_count) {
			snprintf(errbuf, errbuf_len, "graph contains a cycle");
			return false;
		}
		cur = (size_t)nodes[cur].edge_target;
	}
	if (order_count != node_count) {
		snprintf(errbuf, errbuf_len,
			 "graph has %zu node(s) not reachable from the single chain - "
			 "v1 requires every node to be part of it", node_count - order_count);
		return false;
	}

	enum stage_port_type expected_in = PORT_TYPE_RAW_RECORD;
	for (size_t i = 0; i < order_count; i++) {
		struct node_info *n = &nodes[order[i]];
		const struct stage *stage = stage_registry_find(n->type);
		if (stage == NULL) {
			snprintf(errbuf, errbuf_len, "node '%s': unknown stage type '%s'", n->id, n->type);
			goto teardown_and_fail;
		}
		if (stage->max_out_ports != 1) {
			snprintf(errbuf, errbuf_len,
				 "node '%s' (type '%s'): declares %u output port(s), but "
				 "this build's pipeline engine only supports single-output "
				 "stages (max_out_ports == 1) - multi-port routing isn't "
				 "executable yet",
				 n->id, n->type, stage->max_out_ports);
			goto teardown_and_fail;
		}
		if (stage->in_type != expected_in) {
			snprintf(errbuf, errbuf_len,
				 "node '%s' (type '%s'): expects an input this chain doesn't produce here",
				 n->id, n->type);
			goto teardown_and_fail;
		}
		void *state = stage->init(n->config);
		if (state == NULL) {
			snprintf(errbuf, errbuf_len, "node '%s' (type '%s'): failed to initialize - check its config",
				 n->id, n->type);
			goto teardown_and_fail;
		}
		chain->stages[i].stage = stage;
		chain->stages[i].state = state;
		chain->stage_count = i + 1;
		expected_in = stage->out_type;
	}

	if (expected_in != PORT_TYPE_WIRE_FRAME) {
		snprintf(errbuf, errbuf_len,
			 "graph's last stage doesn't produce a wire frame - "
			 "v1 requires the chain to end in a forwarding stage");
		goto teardown_and_fail;
	}

	return true;

teardown_and_fail:
	for (size_t i = 0; i < chain->stage_count; i++)
		if (chain->stages[i].stage->teardown != NULL)
			chain->stages[i].stage->teardown(chain->stages[i].state);
	memset(chain, 0, sizeof(*chain));
	return false;
}
