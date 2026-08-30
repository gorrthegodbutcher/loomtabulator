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
  inType: string;
  outType: string;
  outPortCount: number; // how many source handles StageNode.tsx draws -
                          // an INSTANCE property (depends on this node's
                          // own config, see stage.h's out_port_count),
                          // so it comes from probePortCount() below, not
                          // from stageTypes.ts's per-type listing.
}

export interface GraphMeta {
  loomtabulator_graph_version: number;
  name: string;
  input: Record<string, unknown>; // ring_name/ring_size/record_type - not editable
                                    // in this UI yet, just round-tripped as-is.
}

interface RawGraphNode {
  id: string;
  type: string;
  position?: { x: number; y: number };
  data?: { config?: unknown; label?: string };
}

interface RawGraphEdge {
  id?: string;
  source: string;
  target: string;
  source_port?: number; // optional, default 0 - see graph_config.c/
                          // graph_config.h; omitted whenever it's 0 so
                          // every single-output graph round-trips with
                          // zero schema changes visible on disk.
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
      inType: "none",
      outType: "raw_record",
      outPortCount: 1,
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
 * available", distinct from a real fetch error. */
export async function fetchGraph(stageTypes: StageType[]): Promise<FetchedGraph | null> {
  const res = await fetch("/api/graph");
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
        inType: st?.in_type ?? "unknown",
        outType: st?.out_type ?? "unknown",
        outPortCount: portCounts[i],
      },
    };
  });

  const edges: Edge[] = (raw.edges ?? []).map((e, i) => ({
    id: e.id ?? `e${i}`,
    source: e.source,
    target: e.target,
    sourceHandle: String(e.source_port ?? 0),
  }));

  /* The chain's actual starting node - the one nothing points at (v1's
   * schema requires exactly one, per graph_config.c's own validation,
   * for any graph that ever successfully loaded in the first place). */
  const firstNode = nodes.find((n) => !edges.some((e) => e.target === n.id));
  const ringNode = makeRingInputNode(
    (firstNode?.position.x ?? 0) - 220,
    firstNode?.position.y ?? 100,
  );
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

export async function saveGraph(
  meta: GraphMeta,
  nodes: Node<StageNodeData>[],
  edges: Edge[],
): Promise<{ ok: boolean; error?: string }> {
  /* The ring-input node (see makeRingInputNode() above) is purely a
   * canvas affordance - graph_config.c has no stage type for it and
   * would reject it as "unknown stage type", so it - and whatever edge
   * connects it to the real first stage - never leaves the browser. */
  const realNodes = nodes.filter((n) => n.id !== RING_INPUT_NODE_ID);
  const realEdges = edges.filter(
    (e) => e.source !== RING_INPUT_NODE_ID && e.target !== RING_INPUT_NODE_ID,
  );

  const body: RawGraph = {
    loomtabulator_graph_version: meta.loomtabulator_graph_version,
    name: meta.name,
    input: meta.input,
    nodes: realNodes.map((n) => ({
      id: n.id,
      type: n.data.type,
      position: n.position,
      data: { config: n.data.config, label: n.data.label },
    })),
    edges: realEdges.map((e) => {
      const sourcePort = Number(e.sourceHandle ?? 0);
      return {
        id: e.id,
        source: e.source,
        target: e.target,
        ...(sourcePort !== 0 ? { source_port: sourcePort } : {}),
      };
    }),
  };

  const res = await fetch("/api/graph", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(body),
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
