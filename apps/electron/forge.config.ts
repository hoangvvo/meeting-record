import { AutoUnpackNativesPlugin } from "@electron-forge/plugin-auto-unpack-natives";
import { VitePlugin } from "@electron-forge/plugin-vite";
import type { ForgeConfig } from "@electron-forge/shared-types";
import { rebuild } from "@electron/rebuild";
import { cp, mkdir } from "node:fs/promises";
import { createRequire } from "node:module";
import path from "node:path";

// forge always runs with cwd set to the app directory
const appDir = process.cwd();
const repoRoot = path.resolve(appDir, "../..");
const addonDir = path.join(repoRoot, "node");
const requireFromApp = createRequire(path.join(appDir, "package.json"));
const nodeGypBuildDir = path.dirname(requireFromApp.resolve("node-gyp-build/package.json"));
const electronVersion: string = requireFromApp("electron/package.json").version;

// Permission prompts require a signing identity.
const identity = process.env.MREC_IDENTITY;

// Rebuild in the workspace package, then copy the hoisted dependencies into the app.
const config: ForgeConfig = {
  packagerConfig: {
    name: "mrec-electron",
    appBundleId: "dev.meetingrecord.electron",
    appCategoryType: "public.app-category.developer-tools",
    asar: true,
    extendInfo: {
      // without this key macOS won't even offer the prompt
      NSAudioCaptureUsageDescription: "Records system audio.",
      NSMicrophoneUsageDescription: "Records your microphone.",
      LSMinimumSystemVersion: "14.2",
    },
    osxSign: identity
      ? {
          identity,
          // hardened runtime is on by default and V8 needs the jit / library
          // entitlements to survive it
          optionsForFile: () => ({ entitlements: "entitlements.plist" }),
        }
      : undefined,
  },
  rebuildConfig: {},
  hooks: {
    // Force a rebuild for the current runtime ABI.
    async generateAssets() {
      await rebuild({
        buildPath: appDir,
        projectRootPath: repoRoot,
        electronVersion,
        onlyModules: ["meeting-record"],
        force: true,
      });
    },
    // Keep .forge-meta to prevent rebuilding outside the native source tree.
    async packageAfterCopy(_forgeConfig, buildPath) {
      const target = path.join(buildPath, "node_modules", "meeting-record");
      const entries = [
        "package.json",
        "dist",
        "build/Release/meeting_record.node",
        "build/Release/.forge-meta",
      ];
      for (const entry of entries) {
        const to = path.join(target, entry);
        await mkdir(path.dirname(to), { recursive: true });
        await cp(path.join(addonDir, entry), to, { recursive: true });
      }

      // Copy the hoisted native loader too.
      await cp(nodeGypBuildDir, path.join(buildPath, "node_modules", "node-gyp-build"), {
        recursive: true,
      });
    },
  },
  plugins: [
    // dlopen can't reach a .node inside an asar
    new AutoUnpackNativesPlugin({}),
    new VitePlugin({
      build: [
        { entry: "src/main.ts", config: "vite.main.config.ts", target: "main" },
        {
          entry: "src/preload.ts",
          config: "vite.preload.config.ts",
          target: "preload",
        },
      ],
      renderer: [{ name: "main_window", config: "vite.renderer.config.ts" }],
    }),
  ],
};

export default config;
