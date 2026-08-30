import { defineConfig } from "vite";
import react from "@vitejs/plugin-react";

// Airgap requirement (see CLAUDE.md's Phase 3 design sketch): this build
// must produce a dist/ with zero external network references (no CDN
// fonts, no remote script/style URLs) - `grep -r 'http' dist/` should
// come back empty before this is ever baked into the Docker image.
//
// `server.proxy` only affects `npm run dev`, never the production
// build - it lets the UI be iterated on with `npm run dev` against a
// real running loomtabulator binary without rebuilding web/dist/ and
// restarting the binary on every change.
export default defineConfig({
  plugins: [react()],
  server: {
    proxy: {
      "/api": "http://localhost:8080",
      "/status.json": "http://localhost:8080",
    },
  },
});
