import { useCallback, useEffect, useMemo, useRef, useState } from "react";
import {
  ReactFlow,
  Background,
  Controls,
  MiniMap,
  addEdge,
  applyNodeChanges,
  applyEdgeChanges,
  type Edge,
  type Connection,
  type NodeChange,
  type EdgeChange,
  type Node,
  type NodeMouseHandler,
  useNodesInitialized,
} from "@xyflow/react";
import "@xyflow/react/dist/style.css";
import { fetchStageTypes, type StageType } from "./stageTypes";
import {
  fetchGraph,
  saveGraph,
  saveGraphAs,
  reloadGraph,
  waitForReload,
  fetchStageStatuses,
  makeRingInputNode,
  probePortCount,
  colorForPortType,
  STAGE_NODE_STYLE,
  RING_INPUT_NODE_ID,
  type StageNodeData,
  type GraphMeta,
  type StageStatusEntry,
} from "./graphApi";
import { StageNode } from "./StageNode";
import { StatusPanel } from "./StatusPanel";
import { GraphLibraryDialog } from "./GraphLibraryDialog";

// How often to poll GET /api/stage-status - independent of (and much
// more frequent than the extreme end of) the server's own
// --status-poll-interval default of 2s; a poll landing between two
// server-side updates just re-fetches the same snapshot, harmless.
const STAGE_STATUS_POLL_MS = 3000;

// Defined outside the component - React Flow warns (and pays a
// re-render cost) if nodeTypes is a fresh object every render.
const NODE_TYPES = { stage: StageNode };

// New node ids get this prefix so they can never collide with ids
// loaded from an existing graph (testdata/example_graph.json and
// friends use "n1", "n2", ... - see graph_config.c, which only cares
// that ids are unique and referenced consistently, not their format).
let nextNewNodeId = 1;

const DEFAULT_META: GraphMeta = {
  loomtabulator_graph_version: 1,
  name: "graph",
  input: { ring_name: "LOOM_INPUT_RING", ring_size: 4096 },
};

function App() {
  const [stageTypes, setStageTypes] = useState<StageType[]>([]);
  const [loadError, setLoadError] = useState<string | null>(null);
  const [nodes, setNodes] = useState<Node<StageNodeData>[]>(() => [
    makeRingInputNode(40, 100),
  ]);
  const [edges, setEdges] = useState<Edge[]>([]);
  const [saveStatus, setSaveStatus] = useState<{ kind: "idle" | "saving" | "ok" | "error"; message?: string }>({
    kind: "idle",
  });
  const [saveAsStatus, setSaveAsStatus] = useState<
    { kind: "idle" | "saving" | "ok" | "error"; message?: string }
  >({ kind: "idle" });
  const [reloadStatus, setReloadStatus] = useState<
    { kind: "idle" | "reloading" | "ok" | "error"; message?: string }
  >({ kind: "idle" });
  // No "error" kind here - GraphLibraryDialog surfaces its own Load
  // failures inline (it stays open and never calls onLoaded), so this
  // only ever needs to report a *successful* load, same as onLoaded's
  // own doc comment says.
  const [loadStatus, setLoadStatus] = useState<{ kind: "idle" | "ok"; message?: string }>({
    kind: "idle",
  });
  const [showGraphLibrary, setShowGraphLibrary] = useState(false);
  const [stageStatuses, setStageStatuses] = useState<Record<string, StageStatusEntry>>({});
  const [statusPanelCollapsed, setStatusPanelCollapsed] = useState(false);
  const graphMeta = useRef<GraphMeta>(DEFAULT_META);
  // graphMeta is a ref (mutating it doesn't trigger a re-render, which is
  // fine for its other reader - onSave/onSaveAs only need its CURRENT
  // value at click time) - but the bottom-of-sidebar name display below
  // needs to actually re-render when it changes, so its name is mirrored
  // into this bit of real state at the two places graphMeta.current gets
  // a genuinely different graph assigned (refreshGraphFromServer, New
  // graph) - not at every graphMeta.current mutation (e.g. editing
  // input.position/ring_name via Configure doesn't change .name).
  const [graphName, setGraphName] = useState(DEFAULT_META.name);
  // The graph library filename this canvas actually came from, if the UI
  // has ever been told one (via Load or a successful Save As) - null at
  // startup, since the backend never exposes what --graph=PATH itself
  // was launched with. Used only to default Save As's own filename
  // prompt sensibly - graphMeta.current.name (the graph's own internal
  // "name" JSON field) has no required relationship to its real on-disk
  // filename, so guessing from that instead was actively misleading
  // (suggesting e.g. "v1-example-pipeline.json" for a graph opened from
  // gfp_router_graph.json).
  const [activeGraphFilename, setActiveGraphFilename] = useState<string | null>(null);
  // Captured via <ReactFlow>'s onInit below - lets addStageNode() place a
  // newly-added node at the CURRENT viewport's center (screenToFlowPosition
  // converts screen/client pixels to flow coordinates, accounting for
  // whatever pan/zoom the canvas is currently at) instead of a fixed
  // canvas-coordinate grid, which could land anywhere relative to what's
  // actually visible - including well outside the viewport entirely, the
  // "new node shows up off-screen" bug this fixes. reactFlowContainerRef
  // gives the container's own on-screen bounding rect so "the center" can
  // be computed as real screen pixels in the first place.
  // Only the methods addStageNode()/the fitView effect below actually
  // need - narrowed deliberately rather than the full
  // ReactFlowInstance<NodeType, EdgeType> type, whose other methods
  // (setNodes() in particular) are contravariant in NodeType and fight
  // any generic instantiation that isn't an exact structural match to
  // what <ReactFlow nodes={...}> was actually given.
  const reactFlowInstance = useRef<{
    screenToFlowPosition: (pos: { x: number; y: number }) => { x: number; y: number };
    fitView: (options?: { padding?: number; duration?: number }) => void;
  } | null>(null);
  const reactFlowContainerRef = useRef<HTMLElement>(null);
  const [graphApiEnabled, setGraphApiEnabled] = useState(true);
  const [contextMenu, setContextMenu] = useState<{ nodeId: string; x: number; y: number } | null>(null);
  const [configEditor, setConfigEditor] = useState<{ nodeId: string; text: string; error: string | null } | null>(
    null,
  );
  const [configNotice, setConfigNotice] = useState<string | null>(null);
  // Set by refreshGraphFromServer() whenever it actually loads a graph
  // (startup, or a GraphLibraryDialog Load), consumed by onNodesInitialized
  // below (see <ReactFlow>'s own prop) - fitView() needs every node's real
  // DOM dimensions measured first, which happens asynchronously after
  // React commits new nodes into the DOM, so firing it right after
  // setNodes() (even from a useEffect) can still race React Flow's own
  // internal measurement pass and fit bounds around only whichever nodes
  // happened to be measured yet. onNodesInitialized is React Flow's own
  // "every current node is now measured" signal - the one point it's
  // actually safe to call fitView() and get correct bounds for the WHOLE
  // graph, not just whatever was ready first. A plain ref (not state) -
  // this is consumed inside a callback, not rendered, so it doesn't need
  // to trigger a re-render itself.
  const pendingFitView = useRef(false);

  // Fetches GET /api/graph and populates the canvas from it - shared by
  // the mount-time effect below and by handleGraphLoaded (called after
  // GraphLibraryDialog's Load activates a different graph server-side,
  // so the canvas needs the same refresh treatment startup already gets).
  const refreshGraphFromServer = useCallback(async (types: StageType[]) => {
    const graph = await fetchGraph(types);
    if (graph === null) {
      setGraphApiEnabled(false); // binary running without a web_graph_ctx
      return;
    }
    graphMeta.current = graph.meta;
    setGraphName(graph.meta.name);
    setNodes(graph.nodes);
    setEdges(graph.edges);
    pendingFitView.current = true;
    // A loaded graph can already contain "new-N" ids from an earlier UI
    // session (nextNewNodeId is module-level state, reset to 1 on every
    // fresh page load) - without this, the next addStageNode() call
    // reissues an id already in the graph, and since React Flow indexes
    // nodes by id, the freshly added node silently replaces the existing
    // one at that id instead of appending a new one.
    const maxLoadedNewId = graph.nodes.reduce((max, n) => {
      const m = /^new-(\d+)$/.exec(n.id);
      return m ? Math.max(max, Number(m[1])) : max;
    }, 0);
    nextNewNodeId = Math.max(nextNewNodeId, maxLoadedNewId + 1);

    // A graph file saved before input.position existed has no saved
    // ring-input position - fetchGraph() already computed a reasonable
    // fallback for THIS load (220px left of the first node) and put the
    // ring-input node there. Deliberately NOT auto-saving that back to
    // disk here: an earlier version of this did, in the background,
    // right after load - and that raced any Save the user made shortly
    // after loading (two concurrent POST /api/graph calls, whichever
    // response landed second wins) - if the background save's older,
    // pre-edit snapshot completed after the user's own real edit, it
    // silently clobbered it. No separate fix needed instead: buildRawGraph()
    // already writes the ring-input node's CURRENT position into
    // input.position on every save regardless of whether the file had
    // one before, so this self-heals the moment the user saves anything
    // normally - no race, no extra write path to get wrong.
  }, []);

  useEffect(() => {
    fetchStageTypes()
      .then(async (types) => {
        setStageTypes(types);
        await refreshGraphFromServer(types);
      })
      .catch((err) => setLoadError(String(err)));
  }, [refreshGraphFromServer]);

  // True exactly when every CURRENT node has been measured (has a real
  // DOM width/height) - see pendingFitView's own comment for why this,
  // not a useEffect timed off the state update itself, is the actual
  // signal fitView() needs to wait for to compute correct bounds for the
  // whole graph rather than just whichever nodes happened to be measured
  // first. Requires App to be a descendant of <ReactFlowProvider> (see
  // main.tsx) - unlike reactFlowInstance's onInit-based ref above, this
  // one really does need the hook.
  const nodesInitialized = useNodesInitialized();
  useEffect(() => {
    if (nodesInitialized && pendingFitView.current) {
      pendingFitView.current = false;
      reactFlowInstance.current?.fitView({ padding: 0.2, duration: 200 });
    }
  }, [nodesInitialized]);

  // Live per-stage status (GET /api/stage-status, ABI v6's struct
  // stage.get_status()) - polled on a fixed client-side interval,
  // independent of the server's own --status-poll-interval (a poll
  // landing between two server-side updates just re-fetches the same
  // snapshot). One immediate fetch on mount so the first real data
  // doesn't wait a full interval; cleared on unmount.
  useEffect(() => {
    let cancelled = false;
    const poll = () => {
      fetchStageStatuses().then((result) => {
        if (!cancelled) setStageStatuses(result);
      });
    };
    poll();
    const id = setInterval(poll, STAGE_STATUS_POLL_MS);
    return () => {
      cancelled = true;
      clearInterval(id);
    };
  }, []);

  const onNodesChange = useCallback(
    (changes: NodeChange<Node<StageNodeData>>[]) =>
      setNodes((nds) => applyNodeChanges(changes, nds)),
    [],
  );
  const onEdgesChange = useCallback(
    (changes: EdgeChange[]) => setEdges((eds) => applyEdgeChanges(changes, eds)),
    [],
  );

  const onConnect = useCallback(
    (connection: Connection) => {
      const source = nodes.find((n) => n.id === connection.source);
      const target = nodes.find((n) => n.id === connection.target);
      if (!source || !target || !target.data.inTypes.includes(source.data.outType)) {
        return; // mirrors graph_config.c's port-type membership check
      }
      // mirrors graph_config.c's "output port already has an outgoing
      // edge" check - immediate feedback only, Save remains the
      // authoritative check either way, same posture as the port-type
      // check above.
      const alreadyWired = edges.some(
        (e) => e.source === connection.source && e.sourceHandle === connection.sourceHandle,
      );
      if (alreadyWired) return;
      setEdges((eds) => addEdge(connection, eds));
    },
    [nodes, edges],
  );

  // Where a newly-added node should land: the CURRENT viewport's center,
  // in flow coordinates - screenToFlowPosition() accounts for whatever
  // pan/zoom the canvas is at, so this is always on-screen regardless of
  // where the user has scrolled to, unlike a fixed canvas-coordinate
  // origin (the old behavior, which could - and did - place a new node
  // well outside the visible viewport). Falls back to a fixed point only
  // if the canvas hasn't finished mounting yet (onInit hasn't fired) -
  // shouldn't normally happen, since the palette isn't clickable before
  // the canvas itself renders, but keeps this total rather than throwing.
  const viewportCenterPosition = useCallback((): { x: number; y: number } => {
    const instance = reactFlowInstance.current;
    const container = reactFlowContainerRef.current;
    if (instance == null || container == null) {
      return { x: 80, y: 80 };
    }
    const rect = container.getBoundingClientRect();
    return instance.screenToFlowPosition({
      x: rect.left + rect.width / 2,
      y: rect.top + rect.height / 2,
    });
  }, []);

  const addStageNode = useCallback(async (stage: StageType) => {
    const id = `new-${nextNewNodeId++}`;
    const outPortCount = await probePortCount(stage.name, {});
    const center = viewportCenterPosition();
    setNodes((nds) => [
      ...nds,
      {
        id,
        type: "stage",
        // Small stagger (a short diagonal cascade, wrapping every 6
        // nodes) so several added in quick succession don't land exactly
        // on top of each other and become impossible to tell apart or
        // click individually - still centered on the viewport, not a
        // fixed canvas origin.
        position: { x: center.x - 80 + (nds.length % 6) * 24, y: center.y - 40 + (nds.length % 6) * 24 },
        style: STAGE_NODE_STYLE,
        data: {
          label: `${stage.name} (${id})`,
          type: stage.name,
          config: {},
          inTypes: stage.in_types,
          outType: stage.out_type,
          outPortCount,
          targetConnectedColor: null, // filled in by the useMemo below
          onInvalid: "drop", // default - see StageNodeData's own comment
          liveStatus: null, // filled in by the poll below, once it ticks
        },
      },
    ]);
  }, [viewportCenterPosition]);

  // Also syncs the library copy this canvas is known to have come from
  // (activeGraphFilename - set by Load or a prior Save As, see its own
  // comment), if any - without this, POST /api/graph (the active file)
  // and the library entry silently diverge: Load a library graph, edit
  // it, Save (writes only the active file, POST /api/graph never touches
  // graphs_dir - confirmed in web_status.c), then Load that SAME library
  // entry again later - it re-copies the untouched, stale library file
  // back over the active one, quietly discarding the edit. Syncing both
  // here makes Save behave the way it reads: you opened a named graph,
  // you hit Save, it's actually saved, not just half of it.
  const onSave = useCallback(async () => {
    setSaveStatus({ kind: "saving" });
    const result = await saveGraph(graphMeta.current, nodes, edges);
    if (!result.ok) {
      setSaveStatus({ kind: "error", message: result.error });
      return;
    }
    if (activeGraphFilename != null) {
      const libraryResult = await saveGraphAs(activeGraphFilename, graphMeta.current, nodes, edges);
      if (!libraryResult.ok) {
        setSaveStatus({
          kind: "error",
          message: `Saved the active graph, but failed to sync "${activeGraphFilename}" in the library: ${libraryResult.error}`,
        });
        return;
      }
    }
    setSaveStatus({ kind: "ok" });
  }, [nodes, edges, activeGraphFilename]);

  // Save As: prompts for a filename (forcing ".json", same convention
  // GraphLibraryDialog's own Upload uses for a dropped-in file without
  // one), then uploads the current canvas into the graph library under
  // that name via saveGraphAs() - library only, doesn't touch the
  // active graph (same posture Upload already has; Load afterward makes
  // it active). window.prompt(), same simple-text-input convention
  // handleRename already uses below, rather than a bespoke dialog.
  const onSaveAs = useCallback(async () => {
    const suggested =
      activeGraphFilename ?? (graphMeta.current.name ? `${graphMeta.current.name}.json` : "graph.json");
    const raw = window.prompt("Save as (filename)", suggested);
    if (!raw) return; // cancelled, or an empty name
    const name = raw.endsWith(".json") ? raw : `${raw}.json`;

    setSaveAsStatus({ kind: "saving" });
    const result = await saveGraphAs(name, graphMeta.current, nodes, edges);
    if (result.ok) {
      setActiveGraphFilename(name);
    }
    setSaveAsStatus(
      result.ok
        ? { kind: "ok", message: `Saved as "${name}" in the library.` }
        : { kind: "error", message: result.error },
    );
  }, [nodes, edges, activeGraphFilename]);

  // New Graph: resets the canvas to a blank one (client-side only, no
  // server call - nothing is persisted until Save/Save As). Confirms
  // first if there's anything beyond the synthetic ring-input node to
  // actually lose, same guard Delete-in-library already uses.
  const onNewGraph = useCallback(() => {
    if (nodes.length > 1 && !window.confirm("Start a new graph? Unsaved changes will be lost.")) {
      return;
    }
    graphMeta.current = { ...DEFAULT_META, input: { ...DEFAULT_META.input } };
    setGraphName(DEFAULT_META.name);
    setActiveGraphFilename(null);
    setNodes([makeRingInputNode(40, 100)]);
    setEdges([]);
    setSaveStatus({ kind: "idle" });
    setSaveAsStatus({ kind: "idle" });
  }, [nodes.length]);

  // Reload: POSTs /api/reload (main.c gracefully shuts down and
  // re-exec's itself - see graphApi.ts's own comment), then polls until
  // the server answers again. Not gated on having just saved - reloading
  // with no new save just re-applies the same graph file unchanged,
  // which is a reasonable thing to want on its own (e.g. after fixing
  // something out of band).
  const onReload = useCallback(async () => {
    setReloadStatus({ kind: "reloading" });
    const result = await reloadGraph();
    if (!result.ok) {
      setReloadStatus({ kind: "error", message: result.error });
      return;
    }
    const backUp = await waitForReload();
    setReloadStatus(
      backUp ? { kind: "ok" } : { kind: "error", message: "timed out waiting for it to come back up" },
    );
  }, []);

  // Called by GraphLibraryDialog after a successful Load - the server
  // has already activated the chosen graph (written it to --graph=PATH,
  // same effect Save has), so the canvas needs the same GET /api/graph
  // refresh startup does to actually show it, ahead of the still-manual
  // Reload step that applies it to the running pipeline. `name` is the
  // library filename that was just loaded - see activeGraphFilename's
  // own comment for why this is the one moment the UI can capture it.
  const handleGraphLoaded = useCallback(
    async (name: string) => {
      await refreshGraphFromServer(stageTypes);
      setActiveGraphFilename(name);
      setLoadStatus({ kind: "ok", message: "Loaded - click Reload to apply it." });
    },
    [refreshGraphFromServer, stageTypes],
  );

  // Right-click node menu: Rename/Delete. Rename only ever touches
  // data.label (never data.type/id - those have to stay the real
  // stage_registry.c name and the JSON-referenced node id for save to
  // keep working). Persists through Save the same way position does -
  // graphApi.ts rides it along as an extra data.label key the backend
  // silently ignores - but like position, it's only durable once you
  // actually hit Save; until then it's just in-memory React state, lost
  // on reload same as an unsaved drag would be. Delete removes the node
  // plus any edges touching it; respects `deletable === false` (the
  // ring-input node - see graphApi.ts).
  const onNodeContextMenu: NodeMouseHandler = useCallback((event, node) => {
    event.preventDefault();
    setContextMenu({ nodeId: node.id, x: event.clientX, y: event.clientY });
  }, []);

  const closeContextMenu = useCallback(() => setContextMenu(null), []);

  const handleRename = useCallback((nodeId: string) => {
    const current = nodes.find((n) => n.id === nodeId);
    const next = window.prompt("Rename node", current ? String(current.data.label) : "");
    if (next) {
      setNodes((nds) =>
        nds.map((n) => (n.id === nodeId ? { ...n, data: { ...n.data, label: next } } : n)),
      );
    }
    setContextMenu(null);
  }, [nodes]);

  // Toggles a node's on_invalid between "drop" (default) and "pass" -
  // the two choices that don't require wiring anything extra (the third,
  // routing a flagged record to a dedicated next stage, is just drawing
  // an edge from the node's red "invalid" handle - see StageNode.tsx).
  // Persists through Save the same way label/position already do.
  const handleToggleOnInvalid = useCallback((nodeId: string) => {
    setNodes((nds) =>
      nds.map((n) =>
        n.id === nodeId
          ? { ...n, data: { ...n.data, onInvalid: n.data.onInvalid === "pass" ? "drop" : "pass" } }
          : n,
      ),
    );
    setContextMenu(null);
  }, []);

  const handleDelete = useCallback((nodeId: string) => {
    setNodes((nds) => nds.filter((n) => n.id !== nodeId));
    setEdges((eds) => eds.filter((e) => e.source !== nodeId && e.target !== nodeId));
    setContextMenu(null);
  }, []);

  // Configure: edits a node's data.config as raw JSON - there's no
  // per-stage config schema (a plugin's config shape is opaque to this
  // UI, see graphApi.ts), so a schema-agnostic text editor is the only
  // thing that works for every stage type, including one that lets its
  // OWN config directly set its output port count (e.g. a
  // "num_channels" field, not just an array length). On save, the node
  // is re-probed via POST /api/probe-port-count - the same call
  // addStageNode() already makes - so a config change that alters the
  // port count immediately redraws the right number of handles. Any
  // edge whose source_port no longer fits is dropped automatically
  // (surfaced as a brief notice) rather than left stale on the canvas.
  //
  // The ring-input node is a special case throughout this function and
  // handleSaveConfig below - it's a synthetic canvas anchor (see
  // makeRingInputNode() in graphApi.ts), not a real stage, so it has no
  // data.config/stage type to probe a port count for. It edits
  // graphMeta.current.input (ring_name/ring_size/record_type) instead -
  // the top-level graph field saveGraph() already echoes back verbatim,
  // same "opaque, unvalidated until Save" posture every other node's
  // config already has (graph_config_load() is what actually validates
  // ring_name/ring_size server-side, same as it validates every other
  // node's config).
  const handleOpenConfigure = useCallback(
    (nodeId: string) => {
      if (nodeId === RING_INPUT_NODE_ID) {
        setConfigEditor({ nodeId, text: JSON.stringify(graphMeta.current.input, null, 2), error: null });
        setConfigNotice(null);
        setContextMenu(null);
        return;
      }
      const node = nodes.find((n) => n.id === nodeId);
      if (node) {
        setConfigEditor({ nodeId, text: JSON.stringify(node.data.config ?? {}, null, 2), error: null });
        setConfigNotice(null);
      }
      setContextMenu(null);
    },
    [nodes],
  );

  const handleSaveConfig = useCallback(async () => {
    if (!configEditor) return;

    let parsed: unknown;
    try {
      parsed = JSON.parse(configEditor.text);
    } catch (err) {
      setConfigEditor((ce) => (ce ? { ...ce, error: `Invalid JSON: ${String(err)}` } : ce));
      return;
    }

    if (configEditor.nodeId === RING_INPUT_NODE_ID) {
      graphMeta.current = { ...graphMeta.current, input: parsed as Record<string, unknown> };
      setConfigEditor(null);
      return;
    }
    const parsedConfig = parsed;

    const node = nodes.find((n) => n.id === configEditor.nodeId);
    if (!node) {
      setConfigEditor(null);
      return;
    }

    const outPortCount = await probePortCount(node.data.type, parsedConfig);
    setNodes((nds) =>
      nds.map((n) =>
        n.id === configEditor.nodeId ? { ...n, data: { ...n.data, config: parsedConfig, outPortCount } } : n,
      ),
    );

    const nodeId = configEditor.nodeId;
    const staleEdgeCount = edges.filter(
      (e) => e.source === nodeId && Number(e.sourceHandle ?? 0) >= outPortCount,
    ).length;
    if (staleEdgeCount > 0) {
      setEdges((eds) => eds.filter((e) => !(e.source === nodeId && Number(e.sourceHandle ?? 0) >= outPortCount)));
      setConfigNotice(
        `${node.data.label}: now has ${outPortCount} output port(s) - removed ${staleEdgeCount} edge(s) that no longer fit.`,
      );
    } else {
      setConfigNotice(null);
    }

    setConfigEditor(null);
  }, [configEditor, nodes, edges]);

  const contextMenuNode = contextMenu ? nodes.find((n) => n.id === contextMenu.nodeId) : undefined;

  // Color each edge by the data type actually flowing through it (its
  // source node's outType) rather than one flat accent color, matching
  // StageNode.tsx's handle coloring - a derived render-time transform,
  // never fed back into `edges` state, so it always reflects each
  // node's current outType (e.g. after a Configure edit) with no risk
  // of going stale.
  const coloredEdges = useMemo(
    () =>
      edges.map((e) => {
        const source = nodes.find((n) => n.id === e.source);
        const color = colorForPortType(source?.data.outType ?? "none");
        return { ...e, style: { ...e.style, stroke: color, strokeWidth: 2 } };
      }),
    [edges, nodes],
  );

  // A node's target handle can now accept several types at once (see
  // graphApi.ts's StageNodeData.inTypes), so there's no single
  // obviously-correct color for it while unconnected. Once it has a
  // real incoming edge (at most one - fan-in isn't supported), tint it
  // to match that edge's actual live type, same color coloredEdges
  // already gives the edge itself; StageNode.tsx falls back to a
  // neutral color when this is null. liveStatus rides along in the same
  // pass, for the same reason - both are render-time augmentations of
  // `data`, never fed back into `nodes` state itself, so there's no risk
  // of either going stale or leaking into what saveGraph() sends.
  const nodesWithTargetColor = useMemo(
    () =>
      nodes.map((n) => {
        const incoming = edges.find((e) => e.target === n.id);
        const sourceNode = incoming ? nodes.find((sn) => sn.id === incoming.source) : undefined;
        const targetConnectedColor = sourceNode ? colorForPortType(sourceNode.data.outType) : null;
        const liveStatus = stageStatuses[n.id] ?? null;
        return { ...n, data: { ...n.data, targetConnectedColor, liveStatus } };
      }),
    [nodes, edges, stageStatuses],
  );

  return (
    <div style={{ display: "flex", height: "100vh", background: "var(--bg)" }}>
      <aside
        style={{
          width: 240,
          borderRight: "1px solid var(--border)",
          background: "var(--surface)",
          display: "flex",
          flexDirection: "column",
          // The middle section (Stage types) is the only part that
          // scrolls (flex: 1 + its own overflowY below) - the button
          // block stays pinned at top and the graph-name footer stays
          // pinned at bottom regardless of how many stage types or
          // status messages are showing.
          height: "100vh",
        }}
      >
        <div style={{ padding: 16, flexShrink: 0 }}>
          <button className="btn" onClick={onNewGraph}>
            New graph
          </button>
          <button className="btn" style={{ marginTop: 8 }} onClick={onSave} disabled={!graphApiEnabled || saveStatus.kind === "saving"}>
            {saveStatus.kind === "saving" ? "Saving..." : "Save graph"}
          </button>
          <button
            className="btn"
            style={{ marginTop: 8 }}
            onClick={onSaveAs}
            disabled={!graphApiEnabled || saveAsStatus.kind === "saving"}
          >
            {saveAsStatus.kind === "saving" ? "Saving..." : "Save As..."}
          </button>
          <button
            className="btn"
            style={{ marginTop: 8 }}
            onClick={() => setShowGraphLibrary(true)}
            disabled={!graphApiEnabled}
          >
            Manage graphs...
          </button>
          <button
            className="btn"
            style={{ marginTop: 8 }}
            onClick={onReload}
            disabled={!graphApiEnabled || reloadStatus.kind === "reloading"}
            title="Gracefully restarts loomtabulator, applying whatever graph is currently saved"
          >
            {reloadStatus.kind === "reloading" ? "Reloading..." : "Reload"}
          </button>
          {!graphApiEnabled && (
            <p style={{ fontSize: 11, color: "var(--text-mute)", marginTop: 8 }}>
              Graph API not enabled on this binary (started without --web-port or the graph file
              couldn't be read).
            </p>
          )}
          {saveStatus.kind === "ok" && (
            <p style={{ fontSize: 11, color: "var(--good)", marginTop: 8 }}>
              Saved. Click Reload to apply this graph - saving alone does not affect the currently
              running pipeline.
            </p>
          )}
          {saveStatus.kind === "error" && (
            <p style={{ fontSize: 11, color: "var(--critical)", marginTop: 8, fontFamily: "var(--font-mono)" }}>
              {saveStatus.message}
            </p>
          )}
          {saveAsStatus.kind === "ok" && (
            <p style={{ fontSize: 11, color: "var(--good)", marginTop: 8 }}>{saveAsStatus.message}</p>
          )}
          {saveAsStatus.kind === "error" && (
            <p style={{ fontSize: 11, color: "var(--critical)", marginTop: 8, fontFamily: "var(--font-mono)" }}>
              {saveAsStatus.message}
            </p>
          )}
          {reloadStatus.kind === "reloading" && (
            <p style={{ fontSize: 11, color: "var(--text-mute)", marginTop: 8 }}>
              Restarting loomtabulator and waiting for it to come back up...
            </p>
          )}
          {reloadStatus.kind === "ok" && (
            <p style={{ fontSize: 11, color: "var(--good)", marginTop: 8 }}>
              Reloaded - the new graph is now running.
            </p>
          )}
          {reloadStatus.kind === "error" && (
            <p style={{ fontSize: 11, color: "var(--critical)", marginTop: 8, fontFamily: "var(--font-mono)" }}>
              Reload failed: {reloadStatus.message}
            </p>
          )}
          {loadStatus.kind === "ok" && (
            <p style={{ fontSize: 11, color: "var(--good)", marginTop: 8 }}>{loadStatus.message}</p>
          )}
          {configNotice && (
            <p style={{ fontSize: 11, color: "var(--text-mute)", marginTop: 8 }}>{configNotice}</p>
          )}
        </div>

        <hr style={{ margin: 0, border: "none", borderTop: "1px solid var(--border)" }} />

        <div style={{ padding: 16, flex: 1, overflowY: "auto" }}>
          <p className="card-title" style={{ marginTop: 0 }}>Stage types</p>
          {loadError && (
            <p style={{ color: "var(--critical)", fontSize: 11, fontFamily: "var(--font-mono)" }}>
              Couldn't load from the binary: {loadError}
            </p>
          )}
          {stageTypes.map((stage) => (
            <button
              key={stage.name}
              className="palette-btn"
              onClick={() => addStageNode(stage)}
              title={`${stage.in_types.join(", ")} -> ${stage.out_type}`}
            >
              {stage.name}
            </button>
          ))}
        </div>

        {/* Prefers the real library filename (activeGraphFilename, only
          * known once the user has explicitly Loaded or Save-As'd
          * something - the backend never exposes what --graph=PATH
          * itself was launched with) over graphName (the graph's own
          * internal "name" JSON field, which has no required relation
          * to any real filename - showing that alone was actively
          * misleading, e.g. "v1-example-pipeline" for a graph opened
          * from gfp_router_graph.json). */}
        <div
          style={{
            flexShrink: 0,
            padding: "10px 16px",
            borderTop: "1px solid var(--border)",
            fontSize: 11,
            color: "var(--text-mute)",
            fontFamily: "var(--font-mono)",
            overflow: "hidden",
            textOverflow: "ellipsis",
            whiteSpace: "nowrap",
          }}
          title={activeGraphFilename ?? graphName}
        >
          {activeGraphFilename ?? graphName}
        </div>
      </aside>
      <main ref={reactFlowContainerRef} style={{ flex: 1, position: "relative" }}>
        <ReactFlow
          nodes={nodesWithTargetColor}
          edges={coloredEdges}
          nodeTypes={NODE_TYPES}
          onInit={(instance) => {
            reactFlowInstance.current = instance;
          }}
          onNodesChange={onNodesChange}
          onEdgesChange={onEdgesChange}
          onConnect={onConnect}
          onNodeContextMenu={onNodeContextMenu}
          onPaneClick={closeContextMenu}
          onNodeClick={closeContextMenu}
          onMoveStart={closeContextMenu}
          deleteKeyCode={["Backspace", "Delete"]}
          style={{ background: "var(--bg)" }}
          fitView
        >
          <Background color="var(--border)" gap={16} />
          <Controls />
          <MiniMap
            maskColor="var(--surface-2)"
            nodeColor="var(--accent-soft)"
            nodeStrokeColor="var(--accent)"
          />
        </ReactFlow>
      </main>
      <StatusPanel
        nodes={nodesWithTargetColor}
        collapsed={statusPanelCollapsed}
        onToggleCollapsed={() => setStatusPanelCollapsed((c) => !c)}
      />

      {contextMenu && (
        <>
          <div
            style={{ position: "fixed", inset: 0, zIndex: 999 }}
            onClick={closeContextMenu}
            onContextMenu={(e) => {
              e.preventDefault();
              closeContextMenu();
            }}
          />
          <div className="context-menu" style={{ left: contextMenu.x, top: contextMenu.y }}>
            <button className="context-menu-item" onClick={() => handleRename(contextMenu.nodeId)}>
              Rename
            </button>
            <button
              className="context-menu-item"
              title={
                contextMenuNode?.id === RING_INPUT_NODE_ID
                  ? "Edit the input ring's name/size (graph.input) - not a real stage"
                  : undefined
              }
              onClick={() => handleOpenConfigure(contextMenu.nodeId)}
            >
              Configure
            </button>
            <button
              className="context-menu-item"
              disabled={contextMenuNode?.id === RING_INPUT_NODE_ID}
              title="Cycles between drop (default) and pass - wire the node's red handle instead to route flagged records to a dedicated next stage"
              onClick={() => handleToggleOnInvalid(contextMenu.nodeId)}
            >
              On invalid: {contextMenuNode?.data.onInvalid ?? "drop"}
            </button>
            <button
              className="context-menu-item danger"
              disabled={contextMenuNode?.deletable === false}
              onClick={() => handleDelete(contextMenu.nodeId)}
            >
              Delete
            </button>
          </div>
        </>
      )}

      {showGraphLibrary && (
        <GraphLibraryDialog onClose={() => setShowGraphLibrary(false)} onLoaded={handleGraphLoaded} />
      )}

      {configEditor && (
        <>
          <div
            style={{ position: "fixed", inset: 0, zIndex: 999, background: "rgba(0, 0, 0, 0.4)" }}
            onClick={() => setConfigEditor(null)}
          />
          <div
            style={{
              position: "fixed",
              top: "50%",
              left: "50%",
              transform: "translate(-50%, -50%)",
              zIndex: 1000,
              background: "var(--surface)",
              border: "1px solid var(--border)",
              borderRadius: 10,
              padding: 16,
              width: 420,
              boxShadow: "0 8px 24px rgba(0, 0, 0, 0.3)",
            }}
            onClick={(e) => e.stopPropagation()}
          >
            <p className="card-title" style={{ marginTop: 0 }}>
              Configure: {nodes.find((n) => n.id === configEditor.nodeId)?.data.label ?? configEditor.nodeId}
            </p>
            <p style={{ fontSize: 11, color: "var(--text-mute)", marginTop: -8 }}>
              {configEditor.nodeId === RING_INPUT_NODE_ID
                ? "Raw JSON - the graph's top-level \"input\" (ring_name/ring_size/record_type). OK only " +
                  "applies it to the canvas - nothing is written to disk until you hit Save/Save As."
                : "Raw JSON - this stage's config shape isn't known to the UI. OK re-checks how many output " +
                  "ports this stage has and redraws its handles, but only applies it to the canvas - nothing " +
                  "is written to disk until you hit Save/Save As."}
            </p>
            <textarea
              value={configEditor.text}
              onChange={(e) => setConfigEditor((ce) => (ce ? { ...ce, text: e.target.value, error: null } : ce))}
              rows={12}
              style={{
                width: "100%",
                boxSizing: "border-box",
                fontFamily: "var(--font-mono)",
                fontSize: 12,
                background: "var(--bg)",
                color: "var(--text)",
                border: "1px solid var(--border)",
                borderRadius: 6,
                padding: 8,
                resize: "vertical",
              }}
            />
            {configEditor.error && (
              <p style={{ fontSize: 11, color: "var(--critical)", fontFamily: "var(--font-mono)" }}>
                {configEditor.error}
              </p>
            )}
            <div style={{ display: "flex", justifyContent: "flex-end", gap: 8, marginTop: 8 }}>
              <button className="btn" onClick={() => setConfigEditor(null)}>
                Cancel
              </button>
              <button className="btn" onClick={handleSaveConfig}>
                OK
              </button>
            </div>
          </div>
        </>
      )}
    </div>
  );
}

export default App;
