import { defineConfig } from "vite";

export default defineConfig({
  // don't let vite's output bury rust compiler errors
  clearScreen: false,
  server: {
    port: 1420,
    strictPort: true,
    watch: { ignored: ["**/src-tauri/**"] },
  },
});
