#include <dlfcn.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "plugin_loader.h"
#include "stage_abi.h"

/* Dynamic stage registry - see plugin_loader.h's header comment for the
 * design rationale and stage_abi.h for the ABI contract. Populated
 * entirely by plugin_loader_load(), called once from main.c before
 * graph_config_load() and before any worker thread/lcore exists - the
 * table below is read-only from that point on (query functions are
 * called from graph_config.c on the main lcore at startup, and later
 * from web_status.c's single connection-handling thread), so no
 * locking is needed, same reasoning pipeline_chain's own
 * "read-only after startup" lifecycle already relies on. */
static const struct stage *g_registry[PLUGIN_REGISTRY_MAX];
static void *g_handles[PLUGIN_REGISTRY_MAX];
static size_t g_count;

const struct stage *
stage_registry_find(const char *name)
{
	for (size_t i = 0; i < g_count; i++)
		if (strcmp(g_registry[i]->name, name) == 0)
			return g_registry[i];
	return NULL;
}

size_t
stage_registry_count(void)
{
	return g_count;
}

const struct stage *
stage_registry_get(size_t idx)
{
	return idx < g_count ? g_registry[idx] : NULL;
}

const char *
stage_port_type_name(enum stage_port_type type)
{
	switch (type) {
	case PORT_TYPE_RAW_RECORD:  return "raw_record";
	case PORT_TYPE_ENGINEERING: return "engineering";
	case PORT_TYPE_WIRE_FRAME:  return "wire_frame";
	default:                    return "unknown";
	}
}

/* scandir() filter - only *.so files. scandir()+alphasort() (not raw
 * readdir()) so load order, and therefore the order any log/error
 * output appears in, is deterministic across runs rather than
 * filesystem-dependent - matters for reproducing a reported problem. */
static int
is_so_file(const struct dirent *entry)
{
	size_t len = strlen(entry->d_name);
	return len > 3 && strcmp(entry->d_name + len - 3, ".so") == 0;
}

/* Renders in_types' set bits as a human-readable comma list for the
 * startup load-confirmation log line below - a smaller, log-only
 * cousin of graph_config.c's own describe_accepted_types() (that one
 * feeds a graph-validation error message; this one's just stderr
 * output, so a fixed-size stack buffer is fine either way). Iterates
 * the enum's known sequential range (PORT_TYPE_RAW_RECORD..
 * PORT_TYPE_WIRE_FRAME); update this if stage_port_type ever gains a
 * non-sequential value. */
static void
describe_in_types(unsigned in_types, char *buf, size_t buf_len)
{
	size_t off = 0;
	buf[0] = '\0';
	for (enum stage_port_type t = PORT_TYPE_RAW_RECORD; t <= PORT_TYPE_WIRE_FRAME; t++) {
		if (!(in_types & PORT_TYPE_BIT(t)))
			continue;
		int n = snprintf(buf + off, buf_len - off, "%s%s",
				  off > 0 ? "|" : "", stage_port_type_name(t));
		if (n > 0)
			off += (size_t)n;
	}
}

/* Loads and validates one plugin. Returns false ONLY for a fatal
 * condition (name collision, registry full) - an individual plugin
 * that's malformed in its own right (won't dlopen, missing an export,
 * wrong ABI version, invalid descriptor) is logged and skipped, still
 * returning true, since that shouldn't block graphs that only need
 * OTHER, perfectly good plugins - see plugin_loader.h's own comment. */
static bool
load_one_plugin(const char *path, char *errbuf, size_t errbuf_len)
{
	void *handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
	if (handle == NULL) {
		fprintf(stderr, "loomtabulator: skipping plugin '%s': %s\n", path, dlerror());
		return true;
	}

	loom_stage_abi_version_fn version_fn =
		(loom_stage_abi_version_fn)(void *)dlsym(handle, STAGE_ABI_VERSION_SYMBOL);
	if (version_fn == NULL) {
		fprintf(stderr, "loomtabulator: skipping plugin '%s': missing %s export\n",
			path, STAGE_ABI_VERSION_SYMBOL);
		dlclose(handle);
		return true;
	}
	uint32_t plugin_version = version_fn();
	if (plugin_version != STAGE_ABI_VERSION) {
		fprintf(stderr, "loomtabulator: skipping plugin '%s': ABI version %u, this binary expects %u\n",
			path, plugin_version, STAGE_ABI_VERSION);
		dlclose(handle);
		return true;
	}

	loom_stage_entry_fn entry_fn =
		(loom_stage_entry_fn)(void *)dlsym(handle, STAGE_ABI_ENTRY_SYMBOL);
	if (entry_fn == NULL) {
		fprintf(stderr, "loomtabulator: skipping plugin '%s': missing %s export\n",
			path, STAGE_ABI_ENTRY_SYMBOL);
		dlclose(handle);
		return true;
	}

	const struct stage *stage = entry_fn();
	if (stage == NULL || stage->name == NULL || stage->process == NULL) {
		fprintf(stderr, "loomtabulator: skipping plugin '%s': invalid or NULL stage descriptor\n", path);
		dlclose(handle);
		return true;
	}

	if (stage_registry_find(stage->name) != NULL) {
		snprintf(errbuf, errbuf_len,
			 "plugin '%s' declares stage name '%s', which is already registered "
			 "(by an earlier plugin or a built-in)", path, stage->name);
		dlclose(handle);
		return false;
	}
	if (g_count >= PLUGIN_REGISTRY_MAX) {
		snprintf(errbuf, errbuf_len,
			 "too many stage plugins loaded (max %d) - '%s' didn't fit",
			 PLUGIN_REGISTRY_MAX, path);
		dlclose(handle);
		return false;
	}

	char in_types_desc[128];
	describe_in_types(stage->in_types, in_types_desc, sizeof(in_types_desc));
	fprintf(stderr, "loomtabulator: loaded plugin '%s': stage '%s' (%s -> %s)\n",
		path, stage->name, in_types_desc, stage_port_type_name(stage->out_type));

	g_registry[g_count] = stage;
	g_handles[g_count] = handle;
	g_count++;
	return true;
}

bool
plugin_loader_load(const char *plugins_dir, char *errbuf, size_t errbuf_len)
{
	if (plugins_dir == NULL || plugins_dir[0] == '\0')
		return true;

	struct dirent **entries;
	int n = scandir(plugins_dir, &entries, is_so_file, alphasort);
	if (n < 0) {
		fprintf(stderr, "loomtabulator: plugins directory '%s' not readable (%s) - "
				 "0 plugins loaded\n", plugins_dir, strerror(errno));
		return true;
	}

	bool ok = true;
	for (int i = 0; i < n && ok; i++) {
		char path[PATH_MAX];
		snprintf(path, sizeof(path), "%s/%s", plugins_dir, entries[i]->d_name);
		ok = load_one_plugin(path, errbuf, errbuf_len);
	}
	for (int i = 0; i < n; i++)
		free(entries[i]);
	free(entries);

	return ok;
}

void
plugin_loader_shutdown(void)
{
	for (size_t i = g_count; i > 0; i--)
		dlclose(g_handles[i - 1]);
	g_count = 0;
}
