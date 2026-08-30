import type { Node } from "@xyflow/react";
import { formatStatusValue, type StageNodeData } from "./graphApi";

interface StatusPanelProps {
  nodes: Node<StageNodeData>[];
  collapsed: boolean;
  onToggleCollapsed: () => void;
}

/* Docked right-side live status table (App.tsx's third flex child,
 * alongside the palette <aside> and the canvas <main>) - shows every
 * node that currently has real struct stage.get_status() fields (see
 * graphApi.ts's StageNodeData.liveStatus), refreshed automatically
 * since it's driven entirely by App.tsx's own polled `nodes` prop - no
 * separate polling in here. One card per node rather than a single
 * fixed-column table, since different stage types report different
 * field names/counts - a sparse table would look awkward. Nodes with
 * no status (liveStatus null, or an empty fields array) are simply
 * omitted, not shown empty - most graphs will only have a couple of
 * stages that actually implement get_status(). */
export function StatusPanel({ nodes, collapsed, onToggleCollapsed }: StatusPanelProps) {
  const withStatus = nodes.filter((n) => (n.data.liveStatus?.fields.length ?? 0) > 0);

  return (
    <aside
      style={{
        width: collapsed ? 36 : 240,
        borderLeft: "1px solid var(--border)",
        background: "var(--surface)",
        padding: collapsed ? 8 : 16,
        overflowY: "auto",
        transition: "width 0.15s ease",
        flexShrink: 0,
      }}
    >
      <button
        className="btn"
        onClick={onToggleCollapsed}
        style={{ width: "100%", marginBottom: collapsed ? 0 : 12 }}
        title={collapsed ? "Expand status panel" : "Collapse status panel"}
      >
        {collapsed ? "«" : "Status »"}
      </button>

      {!collapsed && (
        <>
          <p className="card-title">Live status</p>
          {withStatus.length === 0 && (
            <p style={{ fontSize: 11, color: "var(--text-mute)" }}>
              No stage is currently reporting status - see plugin-sdk/README.md's
              "Optional: reporting status" for how a stage opts in.
            </p>
          )}
          {withStatus.map((n) => (
            <div
              key={n.id}
              style={{
                border: "1px solid var(--border)",
                borderRadius: 8,
                padding: "8px 10px",
                marginBottom: 8,
              }}
            >
              <p style={{ fontSize: 11, fontWeight: 600, margin: "0 0 4px", color: "var(--text)" }}>
                {String(n.data.label)}
              </p>
              {n.data.liveStatus?.fields.map((f) => (
                <div
                  key={f.name}
                  style={{
                    display: "flex",
                    justifyContent: "space-between",
                    gap: 8,
                    fontSize: 11,
                    fontFamily: "var(--font-mono)",
                    color: "var(--text-2)",
                  }}
                >
                  <span>{f.name}</span>
                  <span title={f.value.toLocaleString()}>{formatStatusValue(f)}</span>
                </div>
              ))}
            </div>
          ))}
        </>
      )}
    </aside>
  );
}
