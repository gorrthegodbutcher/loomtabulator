import type { ConfigField } from "./stageTypes";

// Renders a stage's config_schema (src/stage.h's struct stage_config_field,
// ABI version 7) as a real form - one control per field by type - instead
// of the raw JSON textarea App.tsx falls back to for a stage with no
// schema. `value` is the config object being edited; `onChange` receives
// the whole updated object on every field change (matching how
// handleSaveConfig already just serializes one config object either way).

const inputStyle: React.CSSProperties = {
  width: "100%",
  boxSizing: "border-box",
  fontFamily: "var(--font-mono)",
  fontSize: 12,
  background: "var(--bg)",
  color: "var(--text)",
  border: "1px solid var(--border)",
  borderRadius: 6,
  padding: "6px 8px",
};

function stringifyValue(v: unknown): string {
  if (v === undefined || v === null) return "";
  return String(v);
}

function parseDefault(field: ConfigField): unknown {
  if (field.default === undefined) return undefined;
  switch (field.type) {
    case "integer":
    case "number":
      return Number(field.default);
    case "boolean":
      return field.default === "true";
    default:
      return field.default;
  }
}

// Merges each field's schema default into `value` wherever it's
// currently unset - called once when a Configure dialog opens (and once
// per row when Add is clicked on an array field), so what the form shows
// is exactly what gets saved: a field with a declared default is
// pre-filled, not silently left absent on the assumption the server will
// apply its own default at parse time. Recurses into array fields' own
// existing rows so nested defaults get the same treatment.
export function applyConfigDefaults(
  fields: ConfigField[],
  value: Record<string, unknown>,
): Record<string, unknown> {
  const result: Record<string, unknown> = { ...value };
  for (const field of fields) {
    if (result[field.name] === undefined) {
      const d = parseDefault(field);
      if (d !== undefined) result[field.name] = d;
    }
    if (field.type === "array" && field.item_fields) {
      const rows = result[field.name];
      if (Array.isArray(rows)) {
        result[field.name] = rows.map((row) =>
          applyConfigDefaults(field.item_fields!, (row ?? {}) as Record<string, unknown>),
        );
      }
    }
  }
  return result;
}

// depends_on_field/depends_on_value (empty depends_on_field = always
// applies) - see stageTypes.ts's own comment. Compares against the
// SIBLING field's current stringified value, same encoding `default`
// already uses (a JSON boolean true compares as "true").
function fieldApplies(field: ConfigField, value: Record<string, unknown>): boolean {
  if (!field.depends_on_field) return true;
  return stringifyValue(value[field.depends_on_field]) === field.depends_on_value;
}

interface ConfigFormProps {
  fields: ConfigField[];
  value: Record<string, unknown>;
  onChange: (value: Record<string, unknown>) => void;
}

export function ConfigForm({ fields, value, onChange }: ConfigFormProps) {
  const setField = (name: string, fieldValue: unknown) => {
    onChange({ ...value, [name]: fieldValue });
  };

  return (
    <div style={{ display: "flex", flexDirection: "column", gap: 10 }}>
      {fields.map((field) => {
        if (!fieldApplies(field, value)) return null;
        return (
          <div key={field.name}>
            <label style={{ display: "block", fontSize: 11, fontWeight: 600, marginBottom: 4 }}>
              {field.name}
              {field.required && <span style={{ color: "var(--critical)" }}> *</span>}
            </label>
            <ConfigFieldControl field={field} value={value} setField={setField} />
            {field.description && (
              <p style={{ fontSize: 10, color: "var(--text-mute)", margin: "2px 0 0" }}>
                {field.description}
              </p>
            )}
          </div>
        );
      })}
    </div>
  );
}

function ConfigFieldControl({
  field,
  value,
  setField,
}: {
  field: ConfigField;
  value: Record<string, unknown>;
  setField: (name: string, v: unknown) => void;
}) {
  const current = value[field.name];

  switch (field.type) {
    case "string":
      return (
        <input
          type="text"
          value={typeof current === "string" ? current : ""}
          onChange={(e) => setField(field.name, e.target.value)}
          style={inputStyle}
        />
      );
    case "integer":
    case "number":
      return (
        <input
          type="number"
          value={typeof current === "number" ? current : ""}
          min={field.min}
          max={field.max}
          step={field.type === "integer" ? 1 : "any"}
          // Empty input -> undefined, which JSON.stringify() omits
          // entirely from the saved config, same "absent means let the
          // stage's own init() apply its default" behavior an untouched
          // field already has server-side.
          onChange={(e) => setField(field.name, e.target.value === "" ? undefined : Number(e.target.value))}
          style={inputStyle}
        />
      );
    case "boolean":
      return (
        <input
          type="checkbox"
          checked={current === true}
          onChange={(e) => setField(field.name, e.target.checked)}
        />
      );
    case "enum":
      return (
        <select
          value={typeof current === "string" ? current : ""}
          onChange={(e) => setField(field.name, e.target.value)}
          style={inputStyle}
        >
          <option value="" disabled>
            select...
          </option>
          {(field.enum_values ?? []).map((v) => (
            <option key={v} value={v}>
              {v}
            </option>
          ))}
        </select>
      );
    case "array":
      return <ArrayFieldControl field={field} value={value} setField={setField} />;
    default:
      return null;
  }
}

// One level of nesting only (an item_fields entry must not itself be
// type "array" - see stageTypes.ts's own comment) - each row is just
// another ConfigForm over that row's own object, recursing exactly once.
function ArrayFieldControl({
  field,
  value,
  setField,
}: {
  field: ConfigField;
  value: Record<string, unknown>;
  setField: (name: string, v: unknown) => void;
}) {
  const rows = Array.isArray(value[field.name]) ? (value[field.name] as Record<string, unknown>[]) : [];
  const itemFields = field.item_fields ?? [];

  const updateRow = (idx: number, newRow: Record<string, unknown>) => {
    const next = rows.slice();
    next[idx] = newRow;
    setField(field.name, next);
  };
  const addRow = () => setField(field.name, [...rows, applyConfigDefaults(itemFields, {})]);
  const removeRow = (idx: number) => setField(field.name, rows.filter((_, i) => i !== idx));

  return (
    <div style={{ border: "1px solid var(--border)", borderRadius: 6, padding: 8 }}>
      {rows.map((row, idx) => (
        <div
          key={idx}
          style={{
            display: "flex",
            alignItems: "flex-start",
            gap: 8,
            marginBottom: 8,
            paddingBottom: 8,
            borderBottom: "1px solid var(--border)",
          }}
        >
          <div style={{ flex: 1 }}>
            <ConfigForm fields={itemFields} value={row} onChange={(newRow) => updateRow(idx, newRow)} />
          </div>
          <button className="btn" style={{ width: "auto", padding: "4px 10px" }} onClick={() => removeRow(idx)}>
            Remove
          </button>
        </div>
      ))}
      {rows.length === 0 && (
        <p style={{ fontSize: 10, color: "var(--text-mute)", margin: "0 0 8px" }}>No entries yet.</p>
      )}
      <button className="btn" style={{ width: "auto", padding: "4px 10px" }} onClick={addRow}>
        Add
      </button>
    </div>
  );
}
