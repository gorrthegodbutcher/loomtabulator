import { useCallback, useEffect, useRef, useState } from "react";
import { deleteStoredGraph, listStoredGraphs, loadStoredGraph, uploadGraph, type StoredGraph } from "./graphApi";

interface GraphLibraryDialogProps {
  onClose: () => void;
  // Called after a successful Load, before the dialog closes itself -
  // App.tsx re-fetches GET /api/graph into the canvas here, the same
  // way it does on initial mount (see its own refreshGraphFromServer).
  // Passed the library filename that was just loaded - the backend never
  // exposes "what file is currently active" any other way (GET /api/graph
  // serves content, not a path), so this is the one moment the UI
  // actually learns a real filename, used to default Save As's own
  // filename prompt sensibly instead of guessing from the graph's
  // unrelated internal "name" JSON field.
  onLoaded: (name: string) => void | Promise<void>;
}

// One consolidated "graph library" dialog covering Upload/Load/Delete,
// following the same overlay+centered-card modal shape App.tsx's own
// config-editor dialog already uses (see App.tsx's configEditor JSX) -
// "Save graph" and "Reload" stay exactly where they are today, outside
// this dialog entirely.
export function GraphLibraryDialog({ onClose, onLoaded }: GraphLibraryDialogProps) {
  const [graphs, setGraphs] = useState<StoredGraph[]>([]);
  const [loading, setLoading] = useState(true);
  const [busyName, setBusyName] = useState<string | null>(null);
  const [error, setError] = useState<string | null>(null);
  const fileInputRef = useRef<HTMLInputElement>(null);

  const refresh = useCallback(async () => {
    setLoading(true);
    const list = await listStoredGraphs();
    setGraphs(list);
    setLoading(false);
  }, []);

  useEffect(() => {
    refresh();
  }, [refresh]);

  const handleUploadClick = useCallback(() => fileInputRef.current?.click(), []);

  const handleFileSelected = useCallback(
    async (e: React.ChangeEvent<HTMLInputElement>) => {
      const file = e.target.files?.[0];
      e.target.value = ""; // allow re-selecting the same file next time
      if (!file) return;

      const text = await file.text();
      const name = file.name.endsWith(".json") ? file.name : `${file.name}.json`;
      setError(null);
      const result = await uploadGraph(name, text);
      if (!result.ok) {
        setError(result.error ?? "upload failed");
        return;
      }
      await refresh();
    },
    [refresh],
  );

  const handleLoad = useCallback(
    async (name: string) => {
      setBusyName(name);
      setError(null);
      const result = await loadStoredGraph(name);
      setBusyName(null);
      if (!result.ok) {
        setError(result.error ?? "load failed");
        return;
      }
      await onLoaded(name);
      onClose();
    },
    [onLoaded, onClose],
  );

  const handleDelete = useCallback(
    async (name: string) => {
      if (!window.confirm(`Delete "${name}"? This cannot be undone.`)) return;
      setBusyName(name);
      setError(null);
      const result = await deleteStoredGraph(name);
      setBusyName(null);
      if (!result.ok) {
        setError(result.error ?? "delete failed");
        return;
      }
      await refresh();
    },
    [refresh],
  );

  return (
    <>
      <div
        style={{ position: "fixed", inset: 0, zIndex: 999, background: "rgba(0, 0, 0, 0.4)" }}
        onClick={onClose}
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
          width: 460,
          maxHeight: "70vh",
          display: "flex",
          flexDirection: "column",
          boxShadow: "0 8px 24px rgba(0, 0, 0, 0.3)",
        }}
        onClick={(e) => e.stopPropagation()}
      >
        <p className="card-title" style={{ marginTop: 0 }}>
          Manage graphs
        </p>
        <p style={{ fontSize: 11, color: "var(--text-mute)", marginTop: -8 }}>
          Load makes a stored graph active (still needs Reload to apply, same as Save). Uploading
          only adds a graph to this list - it doesn't affect what's currently running.
        </p>

        <input
          ref={fileInputRef}
          type="file"
          accept=".json,application/json"
          onChange={handleFileSelected}
          style={{ display: "none" }}
        />
        <button className="btn" onClick={handleUploadClick} style={{ alignSelf: "flex-start" }}>
          Upload...
        </button>

        <div style={{ overflowY: "auto", marginTop: 12, flex: 1 }}>
          {loading && (
            <p style={{ fontSize: 11, color: "var(--text-mute)" }}>Loading...</p>
          )}
          {!loading && graphs.length === 0 && (
            <p style={{ fontSize: 11, color: "var(--text-mute)" }}>No stored graphs yet.</p>
          )}
          {graphs.map((g) => (
            <div
              key={g.name}
              style={{
                display: "flex",
                alignItems: "center",
                justifyContent: "space-between",
                gap: 8,
                padding: "6px 0",
                borderBottom: "1px solid var(--border)",
              }}
            >
              <div style={{ minWidth: 0 }}>
                <div style={{ fontSize: 12, fontFamily: "var(--font-mono)", overflow: "hidden", textOverflow: "ellipsis" }}>
                  {g.name}
                </div>
                <div style={{ fontSize: 10, color: "var(--text-mute)" }}>
                  {new Date(g.mtime * 1000).toLocaleString()}
                </div>
              </div>
              <div style={{ display: "flex", gap: 6, flexShrink: 0 }}>
                <button className="btn" disabled={busyName === g.name} onClick={() => handleLoad(g.name)}>
                  {busyName === g.name ? "..." : "Load"}
                </button>
                <button
                  className="btn"
                  style={{ background: "var(--critical)" }}
                  disabled={busyName === g.name}
                  onClick={() => handleDelete(g.name)}
                >
                  Delete
                </button>
              </div>
            </div>
          ))}
        </div>

        {error && (
          <p style={{ fontSize: 11, color: "var(--critical)", fontFamily: "var(--font-mono)", marginBottom: 0 }}>
            {error}
          </p>
        )}

        <div style={{ display: "flex", justifyContent: "flex-end", marginTop: 8 }}>
          <button className="btn" onClick={onClose}>
            Close
          </button>
        </div>
      </div>
    </>
  );
}
