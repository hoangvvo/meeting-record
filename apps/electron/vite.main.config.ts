import { defineConfig } from "vite";

// Emit .mjs for ESM. Keep the addon external so loading and rebuilding still work.
export default defineConfig({
  build: {
    lib: { entry: "src/main.ts", formats: ["es"], fileName: () => "main.mjs" },
    rollupOptions: { external: ["meeting-record"] },
  },
});
