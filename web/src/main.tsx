import React from "react";
import ReactDOM from "react-dom/client";
import { ReactFlowProvider } from "@xyflow/react";
import App from "./App";
import "./index.css";

// App itself needs to be a descendant of ReactFlowProvider (not just
// <ReactFlow> below it) to call useNodesInitialized() - see App.tsx's
// own comment on why that hook, not a useEffect timed off state, is
// what fitView() actually needs to wait for.
ReactDOM.createRoot(document.getElementById("root")!).render(
  <React.StrictMode>
    <ReactFlowProvider>
      <App />
    </ReactFlowProvider>
  </React.StrictMode>,
);
