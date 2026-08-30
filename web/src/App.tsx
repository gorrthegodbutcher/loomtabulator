import { useCallback, useEffect, useRef, useState } from "react";
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
} from "@xyflow/react";
import "@xyflow/react/dist/style.css";
import { fetchStageTypes, type StageType } from "./stageTypes";
import {
  fetchGraph,
  saveGraph,
  makeRingInputNode,
  STAGE_NODE_STYLE,
  type StageNodeData,
  type GraphMeta,
} from "./graphApi";

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
  const graphMeta = useRef<GraphMeta>(DEFAULT_META);
  const [graphApiEnabled, setGraphApiEnabled] = useState(true);
  const [contextMenu, setContextMenu] = useState<{ nodeId: string; x: number; y: number } | null>(null);

  useEffect(() => {
    fetchStageTypes()
      .then(async (types) => {
        setStageTypes(types);
        const graph = await fetchGraph(types);
        if (graph === null) {
          setGraphApiEnabled(false); // binary running without a web_graph_ctx
          return;
        }
        graphMeta.current = graph.meta;
        setNodes(graph.nodes);
        setEdges(graph.edges);
      })
      .catch((err) => setLoadError(String(err)));
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
      if (!source || !target || source.data.outType !== target.data.inType) {
        return; // mirrors graph_config.c's port-type edge validation
      }
      setEdges((eds) => addEdge(connection, eds));
    },
    [nodes],
  );

  const addStageNode = useCallback((stage: StageType) => {
    const id = `new-${nextNewNodeId++}`;
    setNodes((nds) => [
      ...nds,
      {
        id,
        position: { x: 80 + (nds.length % 5) * 180, y: 80 + Math.floor(nds.length / 5) * 120 },
        style: STAGE_NODE_STYLE,
        data: {
          label: `${stage.name} (${id})`,
          type: stage.name,
          config: {},
          inType: stage.in_type,
          outType: stage.out_type,
        },
      },
    ]);
  }, []);

  const onSave = useCallback(async () => {
    setSaveStatus({ kind: "saving" });
    const result = await saveGraph(graphMeta.current, nodes, edges);
    setSaveStatus(result.ok ? { kind: "ok" } : { kind: "error", message: result.error });
  }, [nodes, edges]);

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

  const handleDelete = useCallback((nodeId: string) => {
    setNodes((nds) => nds.filter((n) => n.id !== nodeId));
    setEdges((eds) => eds.filter((e) => e.source !== nodeId && e.target !== nodeId));
    setContextMenu(null);
  }, []);

  const contextMenuNode = contextMenu ? nodes.find((n) => n.id === contextMenu.nodeId) : undefined;

  return (
    <div style={{ display: "flex", height: "100vh", background: "var(--bg)" }}>
      <aside
        style={{
          width: 240,
          borderRight: "1px solid var(--border)",
          background: "var(--surface)",
          padding: 16,
          overflowY: "auto",
        }}
      >
        <p className="card-title">Stage types</p>
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
            title={`${stage.in_type} -> ${stage.out_type}`}
          >
            {stage.name}
          </button>
        ))}

        <hr style={{ margin: "16px 0", border: "none", borderTop: "1px solid var(--border)" }} />

        <button className="btn" onClick={onSave} disabled={!graphApiEnabled || saveStatus.kind === "saving"}>
          {saveStatus.kind === "saving" ? "Saving..." : "Save graph"}
        </button>
        {!graphApiEnabled && (
          <p style={{ fontSize: 11, color: "var(--text-mute)", marginTop: 8 }}>
            Graph API not enabled on this binary (started without --web-port or the graph file
            couldn't be read).
          </p>
        )}
        {saveStatus.kind === "ok" && (
          <p style={{ fontSize: 11, color: "var(--good)", marginTop: 8 }}>
            Saved. Restart loomtabulator to apply this graph - saving does not affect the
            currently running pipeline.
          </p>
        )}
        {saveStatus.kind === "error" && (
          <p style={{ fontSize: 11, color: "var(--critical)", marginTop: 8, fontFamily: "var(--font-mono)" }}>
            {saveStatus.message}
          </p>
        )}
      </aside>
      <main style={{ flex: 1, position: "relative" }}>
        <ReactFlow
          nodes={nodes}
          edges={edges}
          onNodesChange={onNodesChange}
          onEdgesChange={onEdgesChange}
          onConnect={onConnect}
          onNodeContextMenu={onNodeContextMenu}
          onPaneClick={closeContextMenu}
          onNodeClick={closeContextMenu}
          onMoveStart={closeContextMenu}
          deleteKeyCode={["Backspace", "Delete"]}
          defaultEdgeOptions={{ style: { stroke: "var(--accent)", strokeWidth: 2 } }}
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
              className="context-menu-item danger"
              disabled={contextMenuNode?.deletable === false}
              onClick={() => handleDelete(contextMenu.nodeId)}
            >
              Delete
            </button>
          </div>
        </>
      )}
    </div>
  );
}

export default App;
