import { defineConfig } from "vite";
import react from "@vitejs/plugin-react";

// 产物与 FastAPI 的 mount("/static", ...) + / -> index.html 一致
export default defineConfig({
  plugins: [react()],
  base: "/static/",
  build: {
    outDir: "../static",
    emptyOutDir: true,
  },
  server: {
    port: 5173,
    proxy: {
      "/api": { target: "http://127.0.0.1:8000", changeOrigin: true },
      "/ws": { target: "ws://127.0.0.1:8000", ws: true },
    },
  },
});
