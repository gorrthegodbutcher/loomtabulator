import { useEffect } from "react";
import { Handle, Position, useUpdateNodeInternals, type NodeProps, type Node } from "@xyflow/react";
import { colorForPortType, type StageNodeData } from "./graphApi";

/* Custom node type, replacing React Flow's default one everywhere in
 * App.tsx - the default type only ever renders a single unnamed source
 * handle, but a multi-port stage (out_port_count() > 1, see
 * src/stage.h) needs one distinctly-addressable handle per port so an
 * edge's sourceHandle can record which port it came from (see
 * graphApi.ts's source_port round-trip). A single-port node (the
 * overwhelmingly common case) renders exactly one centered source
 * handle - visually identical to the old default-node look.
 *
 * Box chrome (background/border/radius/padding/font) still comes
 * entirely from the node's own `style` prop (STAGE_NODE_STYLE in
 * graphApi.ts) - React Flow applies `style` to every node's wrapper
 * regardless of node type, so this component only needs to render the
 * label text and the handles themselves. */
export function StageNode({ id, data }: NodeProps<Node<StageNodeData>>) {
  const portCount = Math.max(1, data.outPortCount ?? 1);
  const hasTarget = data.inTypes.length > 0; // the synthetic ring-input node
                                               // (graphApi.ts) has no upstream

  /* React Flow only auto-measures a node's handle positions in the
   * common case of a fixed, static set of handles - since this node's
   * handle COUNT varies per instance (and can change after Configure
   * edits a node's config, see App.tsx's handleSaveConfig), React Flow
   * doesn't know to re-measure on its own. Without this, it silently
   * falls back to the node's center point for any handle it hasn't
   * measured, which looks exactly like "the edge is drawn from the
   * middle of the box instead of the port" - this is a documented
   * gotcha (see useUpdateNodeInternals's own JSDoc example, which is
   * this exact "variable handle count" scenario). */
  const updateNodeInternals = useUpdateNodeInternals();
  useEffect(() => {
    updateNodeInternals(id);
  }, [id, portCount, updateNodeInternals]);

  // The target handle can accept several types at once now (see
  // graphApi.ts's StageNodeData.inTypes), so there's no single color
  // that's obviously "correct" until it's actually wired - App.tsx's
  // targetConnectedColor is null until a real incoming edge exists,
  // in which case it's tinted to match that edge's live type; a hover
  // tooltip lists everything this handle would accept either way.
  // Every source handle carries this node's own outType (all of a
  // node's output ports share one out_type - see stage.h's
  // out_port_count comment), matching App.tsx's per-edge coloring.
  const targetColor = data.targetConnectedColor ?? "var(--border)";
  const sourceColor = colorForPortType(data.outType);

  return (
    <>
      {hasTarget && (
        <Handle
          type="target"
          position={Position.Top}
          title={`accepts: ${data.inTypes.join(", ")}`}
          style={{ background: targetColor, borderColor: targetColor }}
        />
      )}
      <div>{data.label}</div>
      {Array.from({ length: portCount }, (_, k) => (
        <Handle
          key={k}
          type="source"
          position={Position.Bottom}
          id={String(k)}
          title={portCount > 1 ? `port ${k}: ${data.outType}` : data.outType}
          style={{
            left: `${((k + 1) / (portCount + 1)) * 100}%`,
            background: sourceColor,
            borderColor: sourceColor,
          }}
        />
      ))}
    </>
  );
}
