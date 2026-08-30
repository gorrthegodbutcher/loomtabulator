import { Handle, Position, type NodeProps, type Node } from "@xyflow/react";
import type { StageNodeData } from "./graphApi";

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
export function StageNode({ data }: NodeProps<Node<StageNodeData>>) {
  const portCount = Math.max(1, data.outPortCount ?? 1);
  const hasTarget = data.inType !== "none"; // the synthetic ring-input node
                                              // (graphApi.ts) has no upstream

  return (
    <>
      {hasTarget && <Handle type="target" position={Position.Top} />}
      <div>{data.label}</div>
      {Array.from({ length: portCount }, (_, k) => (
        <Handle
          key={k}
          type="source"
          position={Position.Bottom}
          id={String(k)}
          style={{ left: `${((k + 1) / (portCount + 1)) * 100}%` }}
        />
      ))}
    </>
  );
}
