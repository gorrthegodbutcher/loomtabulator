import { useEffect } from "react";
import { Handle, Position, useUpdateNodeInternals, type NodeProps, type Node } from "@xyflow/react";
import { colorForPortType, INVALID_HANDLE_ID, type StageNodeData } from "./graphApi";

/* Custom node type, replacing React Flow's default one everywhere in
 * App.tsx - the default type only ever renders a single unnamed source
 * handle, but a multi-port stage (out_port_count() > 1, see
 * src/stage.h) needs one distinctly-addressable handle per port so an
 * edge's sourceHandle can record which port it came from (see
 * graphApi.ts's source_port round-trip). A single-port node (the
 * overwhelmingly common case) renders exactly one centered source
 * handle - visually identical to the old default-node look. Every real
 * stage node (not the synthetic ring-input one) also renders one more
 * source handle on its right edge, dashed and warning-colored,
 * regardless of port count - the node's dedicated invalid-record path
 * (version 5's on_invalid mechanism, see graphApi.ts's
 * INVALID_HANDLE_ID/StageNodeData.onInvalid comments), deliberately
 * drawn apart from the normal bottom-edge output flow so it doesn't
 * read as "just another port."
 *
 * Box chrome (background/border/radius/padding/font) still comes
 * entirely from the node's own `style` prop (STAGE_NODE_STYLE in
 * graphApi.ts) - React Flow applies `style` to every node's wrapper
 * regardless of node type, so this component only needs to render the
 * label text and the handles themselves. The label also carries a
 * native `title` tooltip built from the node's live
 * struct stage.get_status() fields when it has any (see
 * data.liveStatus's own comment) - the fast, no-new-component path for
 * "hover a stage to see its status"; StatusPanel.tsx is the persistent,
 * always-refreshing counterpart for watching several stages at once. */
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
  // Warning-toned regardless of theme (not one of colorForPortType's
  // type colors - this handle doesn't carry a different data type, just
  // a different DESTINATION for a record this stage flags invalid) -
  // see graphApi.ts's INVALID_HANDLE_ID/StageNodeData.onInvalid comments
  // for the full version-5 mechanism this wires into.
  const invalidColor = "#f43f5e";

  // Native multi-line tooltip (via \n) built from the periodic
  // GET /api/stage-status poll (App.tsx) - see graphApi.ts's
  // StageNodeData.liveStatus comment. undefined (not an empty string)
  // when there's nothing to report, so hovering a node with no status
  // behaves exactly as before this feature existed - no empty tooltip.
  const statusTitle = data.liveStatus?.fields.length
    ? data.liveStatus.fields.map((f) => `${f.name}: ${f.value}`).join("\n")
    : undefined;

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
      <div title={statusTitle}>{data.label}</div>
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
      {hasTarget && (
        <Handle
          type="source"
          position={Position.Right}
          id={INVALID_HANDLE_ID}
          title={`invalid-record path (on_invalid: ${data.onInvalid}${
            data.onInvalid === "pass" ? " - normal path also taken if this is left unwired" : ""
          })`}
          style={{
            background: invalidColor,
            borderColor: invalidColor,
            borderStyle: "dashed",
          }}
        />
      )}
    </>
  );
}
