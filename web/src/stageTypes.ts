// Mirrors src/plugin_loader.c's dynamically-populated registry, served
// over GET /api/stage-types (see src/web_status.c) - the palette and
// edge connection rules below derive entirely from this response,
// never from a client-side hardcoded list, so the UI can't drift out
// of sync with what the binary actually supports (see CLAUDE.md's
// Phase 3 design sketch). in_types is an array, not a single string -
// a stage can accept more than one input type (src/stage.h's
// PORT_TYPE_BIT bitmask), e.g. dump_binary accepts raw_record/
// wire_frame alike.
// Mirrors src/stage.h's struct stage_config_field (ABI version 7) -
// name/type/required/range/enum/default per config field, plus one
// level of array-of-object nesting (item_fields) and a same-schema
// conditional (depends_on_field/depends_on_value - e.g. extract's
// field_width_bytes only applies when mode == "numeric"). Absent
// depends_on_field/depends_on_value means "always applies". A stage
// with no get_config_schema at all omits config_schema from its
// StageType entirely (see StageType.config_schema below) - that's
// distinct from a schema that happens to have zero fields.
export type ConfigFieldType = "string" | "integer" | "number" | "boolean" | "enum" | "array";

export interface ConfigField {
  name: string;
  type: ConfigFieldType;
  required: boolean;
  description?: string;
  min?: number;
  max?: number;
  enum_values?: string[];
  default?: string; // always a string regardless of `type` - see stage.h's own comment
  item_fields?: ConfigField[]; // type === "array" only, one level deep
  depends_on_field?: string;
  depends_on_value?: string;
}

export interface StageType {
  name: string;
  in_types: string[];
  out_type: string;
  config_schema?: ConfigField[];
}

export async function fetchStageTypes(): Promise<StageType[]> {
  const res = await fetch("/api/stage-types");
  if (!res.ok) {
    throw new Error(`GET /api/stage-types failed: ${res.status}`);
  }
  return res.json();
}
