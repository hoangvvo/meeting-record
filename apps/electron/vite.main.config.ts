import { defineConfig } from 'vite'

// meeting-record is ESM-only now, and a CJS main process can't require it, so emit
// ESM. Electron only treats it as ESM if the extension is .mjs, hence fileName.
// The addon stays external: bundling it would break the relative path it uses to
// find meeting_record.node, and @electron/rebuild needs it in node_modules.
export default defineConfig({
  build: {
    lib: { entry: 'src/main.ts', formats: ['es'], fileName: () => 'main.mjs' },
    rollupOptions: { external: ['meeting-record'] },
  },
})
