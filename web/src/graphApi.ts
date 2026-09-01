import type { CSSProperties } from "react";
import type { Node, Edge } from "@xyflow/react";
import type { StageType } from "./stageTypes";

// Mirrors graph_config.c's schema (testdata/example_graph.json) and the
// GET/POST /api/graph contract src/web_status.c implements. POST
// validates and saves to the --graph=PATH file but does NOT affect the
// running pipeline - see web_status.h's header comment for why hot-swap
// wasn't worth the risk to epoch_barrier.c's worker-pool synchronization;
// applying a saved graph needs a restart. There's no per-stage config
// editor yet either (Phase 3 scaffolding only goes as far as adding/
// wiring stage nodes), so a loaded node's `data.config` is carried
// through save unedited - see StageNodeData.config below.
//
// data.label (a custom display name from App.tsx's right-click Rename)
// rides along inside the same "data" object as an extra key alongside
// config - graph_config.c's own parser only ever reads data.config
// (json_object_get(data, "config") - see graph_config.c) and silently
// ignores anything else under "data", the exact same forward-compatible
// pattern "position" already relies on (v1's loader never reads it
// either, but round-trips it so a web-UI-authored layout survives).
// This needs zero backend changes to persist correctly.

export interface StageNodeData extends Record<string, unknown> {
  label: string;      // rendered by StageNode.tsx - without this every
                       // node is a blank box
  type: string;      // plugin_loader.c's stage name - the JSON schema's node "type"
  config: unknown;    // opaque; round-tripped as-is, not editable yet
  inTypes: string[]; // a stage can accept more than one input type (see
                       // stage.h's PORT_TYPE_BIT) - empty means "no target
                       // handle" (only the synthetic ring-input node).
  outType: string;
  outPortCount: number; // how many source handles StageNode.tsx draws -
                          // an INSTANCE property (depends on this node's
                          // own config, see stage.h's out_port_count),
                          // so it comes from probePortCount() below, not
                          // from stageTypes.ts's per-type listing.
  targetConnectedColor: string | null; // computed by App.tsx (a node has
                          // at most one incoming edge) - null means
                          // "no incoming edge yet", so StageNode.tsx
                          // shows a neutral target handle rather than
                          // implying it only accepts one type.
  onInvalid: "drop" | "pass"; // graph_config.c's per-node "on_invalid"
                          // (version 5) - what happens to a record this
                          // node flags via STAGE_RECORD_FLAG_INTEGRITY_
                          // FAILED when its dedicated INVALID_HANDLE_ID
                          // edge below isn't wired. "drop" is the
                          // default and matches every stage's
                          // pre-version-5 behavior, so a loaded graph
                          // that never mentions this round-trips as
                          // "drop" with zero visible change. Toggled via
                          // App.tsx's right-click context menu; inert
                          // for the synthetic ring-input node (it never
                          // flags anything).
  liveStatus: StageStatusEntry | null; // ephemeral, computed at render
                          // time by App.tsx from its periodic
                          // fetchStageStatuses() poll (see below) -
                          // NEVER part of what saveGraph() sends
                          // (it only ever picks config/label/onInvalid
                          // into the POST body, so this needs no extra
                          // exclusion logic). null until the first poll
                          // resolves, or if this node has no status to
                          // report at all.
}

export interface GraphMeta {
  loomtabulator_graph_version: number;
  name: string;
  input: Record<string, unknown>; // ring_name/ring_size/record_type - not editable
                                    // in this UI yet, just round-tripped as-is.
}

// GET /api/stage-status (src/web_status.c's handle_stage_status(), ABI
// v6's struct stage.get_status()) - one entry per graph node, "fields"
// empty when that stage either has no get_status() or currently has
// nothing to report (both normal, not an error).
export interface StageStatusField {
  name: string;
  value: number;
}
export interface StageStatusEntry {
  node_id: string;
  type: string;
  fields: StageStatusField[];
}

const BYTE_UNITS = ["B", "KB", "MB", "GB", "TB", "PB"];

// bytes_written can run up to ~1000 TB - raw digit counts that long are
// unreadable at a glance, so scale to whichever unit keeps the number
// under 1024 (the conventional "1 KB = 1024 B" binary scaling most
// dashboards use, not the strict SI "1 KB = 1000 B").
function formatByteSize(value: number): string {
  let v = value;
  let i = 0;
  while (v >= 1024 && i < BYTE_UNITS.length - 1) {
    v /= 1024;
    i++;
  }
  return `${v.toFixed(i === 0 ? 0 : 2)} ${BYTE_UNITS[i]}`;
}

const ENGINEERING_UNITS: [number, string][] = [
  [1e12, "T"],
  [1e9, "B"],
  [1e6, "M"],
  [1e3, "K"],
];

// Plain counters (records_checked, records_flagged, ...) can run up to
// the trillions - same "scale to a readable magnitude" idea as
// formatByteSize, but with engineering-notation suffixes (K/M/B/T)
// instead of byte units.
function formatEngineering(value: number): string {
  for (const [threshold, suffix] of ENGINEERING_UNITS) {
    if (value >= threshold) return `${(value / threshold).toFixed(2)}${suffix}`;
  }
  return value.toLocaleString();
}

// Shared by StageNode.tsx's hover tooltip and StatusPanel.tsx's table,
// so both surfaces always format the same field the same way. Decided
// purely by the field's own name - not a backend/ABI concept, just a
// display convention - since struct stage_status_field carries no
// "this is a byte count" flag: any field whose name contains "bytes"
// (case-insensitive - "bytes_written", "total_bytes", etc.) gets
// MB/GB/TB-style formatting, everything else gets K/M/B/T
// engineering notation.
export function formatStatusValue(field: StageStatusField): string {
  return /bytes/i.test(field.name) ? formatByteSize(field.value) : formatEngineering(field.value);
}

interface RawGraphNode {
  id: string;
  type: string;
  position?: { x: number; y: number };
  data?: { config?: unknown; label?: string; on_invalid?: string };
}

interface RawGraphEdge {
  id?: string;
  source: string;
  target: string;
  source_port?: number; // optional, default 0 - see graph_config.c/
                          // graph_config.h; omitted whenever it's 0 so
                          // every single-output graph round-trips with
                          // zero schema changes visible on disk.
  invalid_target?: boolean; // version 5 - marks this edge as its source
                          // node's dedicated invalid-record path instead
                          // of an ordinary source_port edge - mutually
                          // exclusive with source_port (see
                          // graph_config.c). Omitted (not just false)
                          // when absent, matching source_port's own
                          // "don't clutter the file" convention.
}

interface RawGraph {
  loomtabulator_graph_version?: number;
  name?: string;
  input?: Record<string, unknown>;
  nodes?: RawGraphNode[];
  edges?: RawGraphEdge[];
}

export interface FetchedGraph {
  meta: GraphMeta;
  nodes: Node<StageNodeData>[];
  edges: Edge[];
}

/* One fixed, categorical color per stage_port_type (src/stage.h's
 * enum) - lets an edge's color (App.tsx) and a node's handle colors
 * (StageNode.tsx) both key off the exact same type name, so "this
 * connector point" and "this line" always visually agree, same as
 * `outType === inType` already has to agree for the connection to be
 * valid in the first place. Deliberately avoids --good/--critical
 * (already mean success/error elsewhere in this UI, not a data type)
 * and --accent (used for other UI chrome, not tied to a single port
 * type) - fixed hex values instead, chosen to stay legible against
 * both index.css themes rather than switching with them. */
const PORT_TYPE_COLORS: Record<string, string> = {
  raw_record: "#8b5cf6",
  engineering: "#ec4899",
  wire_frame: "#06b6d4",
  none: "var(--border)",
};

export function colorForPortType(type: string): string {
  return PORT_TYPE_COLORS[type] ?? "var(--text-mute)";
}

/* Look and feel matched to dpdk-app-example's own status console (its
 * ":root" CSS custom properties, replicated in index.css) - stage boxes
 * use the same surface/border/radius tokens as that page's cards and
 * tiles, just with a bit less corner radius since these are small,
 * dense nodes rather than full page sections. Referencing the CSS
 * variables directly (rather than duplicating literal color values
 * here) means these follow the same light/dark switching index.css
 * already does. */
export const STAGE_NODE_STYLE: CSSProperties = {
  background: "var(--surface)",
  border: "1px solid var(--border)",
  borderRadius: 10,
  color: "var(--text)",
  fontFamily: "var(--font-ui)",
  fontWeight: 600,
  fontSize: 11,
  padding: "10px 14px",
};

/* A synthetic "where does data come from" anchor - doesn't map to any
 * compiled stage.h implementation and is never sent to the backend
 * (graph_config.c has no concept of it - see the filtering in
 * saveGraph() below). Purely a visual entry point for the canvas, since
 * without one there was nothing indicating where the flowchart actually
 * starts. Its outType is "raw_record" (PORT_TYPE_RAW_RECORD) so it
 * plugs into the exact same source.outType === target.inType check
 * App.tsx's onConnect() already does for real stage-to-stage edges -
 * no special-casing needed there. Styled as an accent-tinted variant of
 * the same stage-node look (dashed border, soft accent fill) so it
 * reads as "part of the same system, but not a real stage". */
export const RING_INPUT_NODE_ID = "__ring_input__";
const RING_INPUT_EDGE_ID = "__ring_input_edge__";

/* Source handle id for a node's dedicated invalid-record edge (version
 * 5 - see StageNodeData.onInvalid and graph_config.c's "invalid_target"
 * edge field). Distinct from every numbered port handle id ("0", "1",
 * ...) StageNode.tsx's normal source handles use, so it can share the
 * same Edge.sourceHandle mechanism without colliding - App.tsx's
 * onConnect() "already wired" check and saveGraph()'s round-trip below
 * both just compare handle id strings, no special-casing needed for
 * this one. */
export const INVALID_HANDLE_ID = "invalid";

export function makeRingInputNode(x: number, y: number): Node<StageNodeData> {
  return {
    id: RING_INPUT_NODE_ID,
    type: "stage",
    position: { x, y },
    deletable: false,
    style: {
      ...STAGE_NODE_STYLE,
      background: "var(--accent-soft)",
      border: "2px dashed var(--accent)",
      color: "var(--accent)",
    },
    data: {
      /* Not the graph's raw input.ring_name (e.g. "LOOM_INPUT_RING") -
       * that's a DPDK API identifier, not something a reader unfamiliar
       * with the C code would recognize. This node's whole purpose is
       * to say what it IS in plain terms. */
      label: "\u{1F4E5} DPDK Ring Buffer",
      type: "__ring_input__",
      config: {},
      inTypes: [],
      outType: "raw_record",
      outPortCount: 1,
      targetConnectedColor: null,
      onInvalid: "drop", // inert placeholder - this synthetic node never
                          // flags anything, but StageNodeData requires it
      liveStatus: null, // inert placeholder - not a real stage, never
                          // appears in GET /api/stage-status
    },
  };
}

/* Calls the new POST /api/probe-port-count endpoint (src/web_status.c) -
 * out_port_count() is a per-INSTANCE, config-dependent property (see
 * stage.h), not a fixed property of a stage TYPE the way in_type/
 * out_type are, so this is the only way to know how many handles a
 * node needs before it's ever wired into a real graph. Builds and
 * immediately discards a real stage instance server-side; never
 * touches the actual graph. Falls back to 1 (the overwhelmingly common
 * case) on any failure - a wrong guess here is only a cosmetic
 * handle-count issue, since the real graph_config_load() at Save time
 * remains authoritative either way. */
export async function probePortCount(type: string, config: unknown): Promise<number> {
  try {
    const res = await fetch("/api/probe-port-count", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ type, config }),
    });
    if (!res.ok) return 1;
    const json: { port_count?: number } = await res.json();
    return typeof json.port_count === "number" && json.port_count > 0 ? json.port_count : 1;
  } catch {
    return 1;
  }
}

/* GET /api/graph returns 501 if the binary wasn't wired up with a
 * web_graph_ctx (see web_status.h) - null here means "no graph API
 * available", distinct from a real fetch error.
 *
 * cache: "no-store" is required, not optional - web_status.c's
 * send_response() never sends a Cache-Control header at all, so without
 * this the browser's own default HTTP caching heuristics can serve a
 * STALE response here: call this again right after activating a
 * different graph (POST /api/graphs/load, see handleGraphLoaded in
 * App.tsx) and a cached copy of whatever was fetched earlier in the
 * page's lifetime can come back instead of the server's actual current
 * content - the canvas (and this graph's own .name) then silently don't
 * match what's really active. waitForReload() below already knew to add
 * this; this call needs it even more, since it's not just polling for
 * "is the server back up" but reading content that has to be accurate. */
export async function fetchGraph(stageTypes: StageType[]): Promise<FetchedGraph | null> {
  const res = await fetch("/api/graph", { cache: "no-store" });
  if (res.status === 501) return null;
  if (!res.ok) throw new Error(`GET /api/graph failed: ${res.status}`);

  const raw: RawGraph = await res.json();
  const typeByName = new Map(stageTypes.map((t) => [t.name, t]));

  const rawNodes = raw.nodes ?? [];
  const portCounts = await Promise.all(
    rawNodes.map((n) => probePortCount(n.type, n.data?.config ?? {})),
  );
  const nodes: Node<StageNodeData>[] = rawNodes.map((n, i) => {
    const st = typeByName.get(n.type);
    return {
      id: n.id,
      type: "stage",
      position: n.position ?? { x: 0, y: 0 },
      style: STAGE_NODE_STYLE,
      data: {
        label: n.data?.label ?? `${n.type} (${n.id})`,
        type: n.type,
        config: n.data?.config ?? {},
        inTypes: st?.in_types ?? [],
        outType: st?.out_type ?? "unknown",
        outPortCount: portCounts[i],
        targetConnectedColor: null, // filled in by App.tsx's useMemo
        onInvalid: n.data?.on_invalid === "pass" ? "pass" : "drop",
        liveStatus: null, // filled in by App.tsx's poll, same as targetConnectedColor
      },
    };
  });

  const edges: Edge[] = (raw.edges ?? []).map((e, i) => ({
    id: e.id ?? `e${i}`,
    source: e.source,
    target: e.target,
    sourceHandle: e.invalid_target ? INVALID_HANDLE_ID : String(e.source_port ?? 0),
  }));

  /* The chain's actual starting node - the one nothing points at (v1's
   * schema requires exactly one, per graph_config.c's own validation,
   * for any graph that ever successfully loaded in the first place). */
  const firstNode = nodes.find((n) => !edges.some((e) => e.target === n.id));

  // A user-placed position for the ring-input node, round-tripped through
  // input.position by buildRawGraph() below - graph_config.c never reads
  // this key (only ring_name/ring_size/record_type), same "extra key the
  // backend silently ignores" pattern data.label/position already use for
  // real nodes (see StageNodeData's own comment). Falls back to the old
  // "220px left of the first node" placement only when no saved position
  // exists yet (a graph saved before this existed, or a brand-new one) -
  // without this, the ring-input node used to snap back to that computed
  // spot on every reload regardless of where it had been dragged to.
  const savedPosition = raw.input?.position;
  const ringNode =
    savedPosition != null &&
    typeof savedPosition === "object" &&
    typeof (savedPosition as { x?: unknown }).x === "number" &&
    typeof (savedPosition as { y?: unknown }).y === "number"
      ? makeRingInputNode((savedPosition as { x: number }).x, (savedPosition as { y: number }).y)
      : makeRingInputNode((firstNode?.position.x ?? 0) - 220, firstNode?.position.y ?? 100);
  const ringEdge: Edge[] = firstNode
    ? [{ id: RING_INPUT_EDGE_ID, source: RING_INPUT_NODE_ID, target: firstNode.id }]
    : [];

  return {
    meta: {
      loomtabulator_graph_version: raw.loomtabulator_graph_version ?? 1,
      name: raw.name ?? "graph",
      input: raw.input ?? {},
    },
    nodes: [ringNode, ...nodes],
    edges: [...ringEdge, ...edges],
  };
}

/* Builds the RawGraph JSON this backend actually understands out of the
 * canvas's own React Flow state - shared by saveGraph() (POSTs it to
 * the active graph) and saveGraphAs() below (POSTs it into the library
 * under a new name instead) so "what the canvas currently looks like"
 * has exactly one serialization, used either way. */
function buildRawGraph(meta: GraphMeta, nodes: Node<StageNodeData>[], edges: Edge[]): RawGraph {
  /* The ring-input node (see makeRingInputNode() above) is purely a
   * canvas affordance - graph_config.c has no stage type for it and
   * would reject it as "unknown stage type", so it - and whatever edge
   * connects it to the real first stage - never leaves the browser. Its
   * POSITION does leave the browser, though - see fetchGraph()'s own
   * comment on why input.position exists and how it's read back. */
  const ringInputNode = nodes.find((n) => n.id === RING_INPUT_NODE_ID);
  const input = ringInputNode
    ? { ...meta.input, position: { x: ringInputNode.position.x, y: ringInputNode.position.y } }
    : meta.input;

  const realNodes = nodes.filter((n) => n.id !== RING_INPUT_NODE_ID);
  const realEdges = edges.filter(
    (e) => e.source !== RING_INPUT_NODE_ID && e.target !== RING_INPUT_NODE_ID,
  );

  return {
    loomtabulator_graph_version: meta.loomtabulator_graph_version,
    name: meta.name,
    input,
    nodes: realNodes.map((n) => ({
      id: n.id,
      type: n.data.type,
      position: n.position,
      data: {
        config: n.data.config,
        label: n.data.label,
        ...(n.data.onInvalid === "pass" ? { on_invalid: "pass" } : {}),
      },
    })),
    edges: realEdges.map((e) => {
      if (e.sourceHandle === INVALID_HANDLE_ID) {
        return { id: e.id, source: e.source, target: e.target, invalid_target: true };
      }
      const sourcePort = Number(e.sourceHandle ?? 0);
      return {
        id: e.id,
        source: e.source,
        target: e.target,
        ...(sourcePort !== 0 ? { source_port: sourcePort } : {}),
      };
    }),
  };
}

export async function saveGraph(
  meta: GraphMeta,
  nodes: Node<StageNodeData>[],
  edges: Edge[],
): Promise<{ ok: boolean; error?: string }> {
  const res = await fetch("/api/graph", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(buildRawGraph(meta, nodes, edges)),
  });

  let json: { ok?: boolean; error?: string } = {};
  try {
    json = await res.json();
  } catch {
    /* non-JSON response (e.g. a 501 text/plain) - fall through to the
     * res.ok/status-based error below */
  }

  if (!res.ok || json.ok !== true) {
    return { ok: false, error: json.error ?? `HTTP ${res.status}` };
  }
  return { ok: true };
}

/* "Save As": same canvas serialization as saveGraph() above, but into
 * the graph library under `name` (via uploadGraph()'s POST /api/graphs)
 * rather than overwriting the active graph - same "library only, not
 * activated" posture GraphLibraryDialog's own Upload already has. Use
 * Load afterward to make it the active graph, same two-step flow
 * uploading a file from disk already requires. */
export async function saveGraphAs(
  name: string,
  meta: GraphMeta,
  nodes: Node<StageNodeData>[],
  edges: Edge[],
): Promise<{ ok: boolean; error?: string }> {
  return uploadGraph(name, JSON.stringify(buildRawGraph(meta, nodes, edges)));
}

/* POST /api/reload (src/web_status.c's handle_post_reload()) - triggers
 * a full, graceful restart of the running binary (drain the ring, join
 * every worker, tear down every stage, then main.c re-exec's itself),
 * applying whatever graph is currently saved at --graph=PATH. Same PID
 * throughout on the server side - nothing for this UI to track besides
 * "did the request succeed, and is the server back up yet" (see
 * waitForReload() below). */
export async function reloadGraph(): Promise<{ ok: boolean; error?: string }> {
  try {
    const res = await fetch("/api/reload", { method: "POST" });
    let json: { ok?: boolean } = {};
    try {
      json = await res.json();
    } catch {
      /* non-JSON response - fall through to the res.ok-based error below */
    }
    if (!res.ok || json.ok !== true) {
      return { ok: false, error: `HTTP ${res.status}` };
    }
    return { ok: true };
  } catch (err) {
    return { ok: false, error: String(err) };
  }
}

/* Polls GET /api/graph until it answers again (confirming the reloaded
 * instance is back up and serving) or timeoutMs elapses. The server
 * closes its listening socket during the shutdown-then-re-exec window,
 * so a fetch failing outright (connection refused) during that brief
 * gap is expected, not an error - only a real timeout is. */
export async function waitForReload(timeoutMs = 15000, intervalMs = 300): Promise<boolean> {
  const deadline = Date.now() + timeoutMs;
  while (Date.now() < deadline) {
    try {
      const res = await fetch("/api/graph", { cache: "no-store" });
      if (res.ok || res.status === 501) return true;
    } catch {
      /* listener down between the old instance's shutdown and the new
       * one's startup - keep polling */
    }
    await new Promise((resolve) => setTimeout(resolve, intervalMs));
  }
  return false;
}

// One entry in the graph library (src/web_status.c's GET /api/graphs) -
// distinct from GraphMeta/FetchedGraph above, which describe the one
// currently *active* graph, not this directory listing.
export interface StoredGraph {
  name: string;
  mtime: number; // unix seconds - src/web_status.c's stat().st_mtime
}

/* GET /api/graphs - the named graphs sitting in --graphs-dir, sorted by
 * name (same order the server already returns). Never throws - an empty
 * list is indistinguishable from "the fetch failed," which is fine here
 * since the only caller (GraphLibraryDialog.tsx) just renders whatever
 * comes back and lets the user retry via the dialog's own reopen. */
export async function listStoredGraphs(): Promise<StoredGraph[]> {
  try {
    const res = await fetch("/api/graphs", { cache: "no-store" });
    if (!res.ok) return [];
    return await res.json();
  } catch {
    return [];
  }
}

/* POST /api/graphs?name=X - validates jsonText exactly as saveGraph()'s
 * POST /api/graph does, and on success adds it to the library as `name`
 * (creates or overwrites). Does NOT touch the active graph - see
 * web_status.h's header comment on struct web_graph_ctx.graphs_dir. */
export async function uploadGraph(
  name: string,
  jsonText: string,
): Promise<{ ok: boolean; error?: string }> {
  try {
    const res = await fetch(`/api/graphs?name=${encodeURIComponent(name)}`, {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: jsonText,
    });
    let json: { ok?: boolean; error?: string } = {};
    try {
      json = await res.json();
    } catch {
      /* non-JSON response - fall through to the res.ok-based error below */
    }
    if (!res.ok || json.ok !== true) {
      return { ok: false, error: json.error ?? `HTTP ${res.status}` };
    }
    return { ok: true };
  } catch (err) {
    return { ok: false, error: String(err) };
  }
}

/* POST /api/graphs/load?name=X - (re-)validates a library graph and
 * activates it (same "writes to --graph=PATH, restart_required: true"
 * effect saveGraph() has). The caller (GraphLibraryDialog.tsx, via
 * App.tsx's onLoaded prop) is responsible for re-fetching /api/graph
 * afterward so the canvas reflects the newly-active graph. */
export async function loadStoredGraph(name: string): Promise<{ ok: boolean; error?: string }> {
  try {
    const res = await fetch(`/api/graphs/load?name=${encodeURIComponent(name)}`, {
      method: "POST",
    });
    let json: { ok?: boolean; error?: string } = {};
    try {
      json = await res.json();
    } catch {
      /* non-JSON response - fall through to the res.ok-based error below */
    }
    if (!res.ok || json.ok !== true) {
      return { ok: false, error: json.error ?? `HTTP ${res.status}` };
    }
    return { ok: true };
  } catch (err) {
    return { ok: false, error: String(err) };
  }
}

/* DELETE /api/graphs?name=X - removes one graph from the library. Never
 * touches the active graph, even if that graph happens to be the one
 * being deleted here - see web_status.h's header comment. */
export async function deleteStoredGraph(name: string): Promise<{ ok: boolean; error?: string }> {
  try {
    const res = await fetch(`/api/graphs?name=${encodeURIComponent(name)}`, {
      method: "DELETE",
    });
    let json: { ok?: boolean; error?: string } = {};
    try {
      json = await res.json();
    } catch {
      /* non-JSON response - fall through to the res.ok-based error below */
    }
    if (!res.ok || json.ok !== true) {
      return { ok: false, error: json.error ?? `HTTP ${res.status}` };
    }
    return { ok: true };
  } catch (err) {
    return { ok: false, error: String(err) };
  }
}

/* GET /api/stage-status (src/web_status.c's handle_stage_status()) -
 * every node's current struct stage.get_status() snapshot, keyed by
 * node_id for O(1) lookup against the graph's own node ids. Called on
 * an interval by App.tsx (see its own useEffect) - deliberately slow
 * (--status-poll-interval, default 2s server-side), so a transient
 * failure here just means "try again next tick," not something worth
 * surfacing as an error in the UI. */
export async function fetchStageStatuses(): Promise<Record<string, StageStatusEntry>> {
  try {
    const res = await fetch("/api/stage-status", { cache: "no-store" });
    if (!res.ok) return {};
    const entries: StageStatusEntry[] = await res.json();
    return Object.fromEntries(entries.map((e) => [e.node_id, e]));
  } catch {
    return {};
  }
}
