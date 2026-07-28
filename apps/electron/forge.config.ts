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
const nodeGypBuildDir = path.dirname(
  requireFromApp.resolve("node-gyp-build/package.json"),
);
const electronVersion: string = requireFromApp("electron/package.json").version;

// An ad-hoc signature (`codesign -s -`) gets you no TCC prompt at all; an Apple
// Development cert does. Never hardcode it.
const identity = process.env.MREC_IDENTITY;

// npm always links workspace packages into the *root* node_modules, whatever
// install-strategy you pick, so this app's node_modules is empty and packager
// copies no addon. Hence the two hooks below. Related: the addon can only be
// compiled inside its own tree, since binding.gyp reaches ../native/build for the
// Swift static lib.
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
    // runs for both `start` and `package`. force, because `npm run build`
    // leaves a node-ABI build behind and a stale .forge-meta would let it through
    async generateAssets() {
      await rebuild({
        buildPath: appDir,
        projectRootPath: repoRoot,
        electronVersion,
        onlyModules: ["meeting-record"],
        force: true,
      });
    },
    // .forge-meta comes along so forge's own rebuild pass recognises the copy as
    // already built and skips it. it would fail here, away from ../native/build
    //
    // @see https://github.com/electron/forge/pull/4232
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

      // meeting-record loads its native binary through this production
      // dependency. npm hoists it to the workspace root too, so Forge does not
      // discover it while walking this app's otherwise-empty node_modules.
      await cp(
        nodeGypBuildDir,
        path.join(buildPath, "node_modules", "node-gyp-build"),
        {
          recursive: true,
        },
      );
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
