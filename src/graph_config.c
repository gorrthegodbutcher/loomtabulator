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
	int port_target[STAGE_MAX_OUT_PORTS]; /* port_target[k] = index into
						  nodes[] that this node's
						  output port k routes to,
						  -1 if port k has no outgoing
						  edge (yet, or ever) */
	enum stage_port_type expected_in; /* Set once, before this node is
					      enqueued for processing below -
					      PORT_TYPE_RAW_RECORD for the
					      single root, otherwise set by
					      this node's one and only parent
					      (in_degree <= 1 is enforced
					      below) to that parent's
					      out_type. */
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
		for (unsigned k = 0; k < STAGE_MAX_OUT_PORTS; k++)
			nodes[i].port_target[k] = -1;
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
		/* Optional - omitted (or 0) means "this node's sole/first output
		 * port," so every existing single-output graph keeps loading
		 * unchanged with zero edits. */
		double source_port_raw = json_as_number(json_object_get(edge, "source_port"), 0);
		if (source_port_raw < 0 || source_port_raw >= STAGE_MAX_OUT_PORTS) {
			snprintf(errbuf, errbuf_len,
				 "edge %zu: source_port %g is out of range [0, %d)",
				 e, source_port_raw, STAGE_MAX_OUT_PORTS);
			return false;
		}
		unsigned source_port = (unsigned)source_port_raw;
		if (nodes[src_idx].port_target[source_port] != -1) {
			snprintf(errbuf, errbuf_len,
				 "node '%s': output port %u already has an outgoing edge - "
				 "a stage's output port can only be wired to one downstream node",
				 nodes[src_idx].id, source_port);
			return false;
		}
		nodes[src_idx].port_target[source_port] = (int)dst_idx;
		nodes[dst_idx].in_degree++;
	}

	/* No fan-in: every node has at most one parent, so the graph is a
	 * tree (rooted at the one node with no incoming edge), not a general
	 * DAG - see stage.h's out_port_count comment for why fan-OUT (one
	 * node, several outgoing edges on distinct ports) is fine but this
	 * isn't. */
	size_t start_idx = SIZE_MAX;
	for (size_t i = 0; i < node_count; i++) {
		if (nodes[i].in_degree > 1) {
			snprintf(errbuf, errbuf_len,
				 "node '%s' has more than one incoming edge - "
				 "this build's pipeline engine doesn't support merging "
				 "multiple upstream paths into one node", nodes[i].id);
			return false;
		}
		if (nodes[i].in_degree == 0) {
			if (start_idx != SIZE_MAX) {
				snprintf(errbuf, errbuf_len,
					 "graph has more than one starting node ('%s' and '%s') - "
					 "exactly one node must have no incoming edge",
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

	/* Breadth-first from the root: a node is only ever enqueued once
	 * (its one and only parent is the sole possible enqueuer, since
	 * in_degree <= 1 was just enforced above), so `tail` can never
	 * exceed node_count and this queue can never overflow MAX_NODES -
	 * no separate cycle-detection pass is needed the way v1's flat-array
	 * walk needed one. A node's chain slot is its own original nodes[]
	 * index (no remapping) - graph_config_load() already
	 * memset(chain, 0, ...) up front, so chain->stages[i].stage != NULL
	 * is already a reliable "this slot is populated" signal regardless
	 * of visitation order, which is what lets teardown_and_fail below
	 * scan 0..node_count instead of needing a contiguous prefix. */
	nodes[start_idx].expected_in = PORT_TYPE_RAW_RECORD;
	size_t queue[MAX_NODES];
	size_t head = 0, tail = 0;
	queue[tail++] = start_idx;

	while (head < tail) {
		size_t idx = queue[head++];
		struct node_info *n = &nodes[idx];
		const struct stage *stage = stage_registry_find(n->type);
		if (stage == NULL) {
			snprintf(errbuf, errbuf_len, "node '%s': unknown stage type '%s'", n->id, n->type);
			goto teardown_and_fail;
		}
		if (stage->in_type != n->expected_in) {
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

		unsigned port_count = stage->out_port_count != NULL ? stage->out_port_count(state) : 1;
		if (port_count > STAGE_MAX_OUT_PORTS) {
			snprintf(errbuf, errbuf_len,
				 "node '%s' (type '%s'): declares %u output port(s), exceeds "
				 "this build's cap of %d", n->id, n->type, port_count, STAGE_MAX_OUT_PORTS);
			/* Not yet recorded into chain->stages[], so the
			 * teardown_and_fail sweep below doesn't know about it -
			 * tear this one down directly or it leaks. */
			if (stage->teardown != NULL)
				stage->teardown(state);
			goto teardown_and_fail;
		}
		if (port_count == 0 && stage->out_type != PORT_TYPE_WIRE_FRAME) {
			snprintf(errbuf, errbuf_len,
				 "node '%s' (type '%s') is a leaf (declares 0 output ports) "
				 "but doesn't produce a wire frame - every leaf must end in "
				 "a forwarding stage", n->id, n->type);
			if (stage->teardown != NULL)
				stage->teardown(state);
			goto teardown_and_fail;
		}

		chain->stages[idx].stage = stage;
		chain->stages[idx].state = state;
		chain->stages[idx].port_count = port_count;
		for (unsigned k = 0; k < STAGE_MAX_OUT_PORTS; k++)
			chain->stages[idx].children[k] = -1;

		for (unsigned k = 0; k < STAGE_MAX_OUT_PORTS; k++) {
			bool wired = n->port_target[k] != -1;
			if (k < port_count) {
				if (!wired) {
					snprintf(errbuf, errbuf_len,
						 "node '%s' (type '%s'): output port %u has no "
						 "outgoing edge but this stage declares %u port(s)",
						 n->id, n->type, k, port_count);
					goto teardown_and_fail;
				}
				chain->stages[idx].children[k] = n->port_target[k];
				nodes[n->port_target[k]].expected_in = stage->out_type;
				queue[tail++] = (size_t)n->port_target[k];
			} else if (wired) {
				snprintf(errbuf, errbuf_len,
					 "node '%s' (type '%s'): wires output port %u but only "
					 "declares %u port(s)", n->id, n->type, k, port_count);
				goto teardown_and_fail;
			}
		}
	}

	if (tail != node_count) {
		snprintf(errbuf, errbuf_len,
			 "graph has %zu node(s) not reachable from the single starting node",
			 node_count - tail);
		goto teardown_and_fail;
	}

	chain->stage_count = node_count;
	chain->root_idx = start_idx;
	return true;

teardown_and_fail:
	for (size_t i = 0; i < node_count; i++)
		if (chain->stages[i].stage != NULL && chain->stages[i].stage->teardown != NULL)
			chain->stages[i].stage->teardown(chain->stages[i].state);
	memset(chain, 0, sizeof(*chain));
	return false;
}
