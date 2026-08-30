// Mirrors src/plugin_loader.c's dynamically-populated registry, served
// over GET /api/stage-types (see src/web_status.c) - the palette and
// edge connection rules below derive entirely from this response,
// never from a client-side hardcoded list, so the UI can't drift out
// of sync with what the binary actually supports (see CLAUDE.md's
// Phase 3 design sketch). in_types is an array, not a single string -
// a stage can accept more than one input type (src/stage.h's
// PORT_TYPE_BIT bitmask), e.g. dump_binary accepts raw_record/
// wire_frame alike.
export interface StageType {
  name: string;
  in_types: string[];
  out_type: string;
}

export async function fetchStageTypes(): Promise<StageType[]> {
  const res = await fetch("/api/stage-types");
  if (!res.ok) {
    throw new Error(`GET /api/stage-types failed: ${res.status}`);
  }
  return res.json();
}
