//! Links the native core.
//!
//! The macOS core is Swift, so it is built by scripts/build-macos.sh rather than
//! by cargo. This only tells the linker where to find it and which system
//! frameworks it needs.

use std::env;
use std::path::PathBuf;
use std::process::Command;

#[cfg(target_os = "windows")]
use cc::Build;

fn main() {
    let manifest = PathBuf::from(env::var("CARGO_MANIFEST_DIR").unwrap());
    let root = manifest.parent().unwrap().to_path_buf();

    if cfg!(target_os = "macos") {
        let lib_dir = root.join("native/build");

        // Build the Swift core if it is missing, so `cargo build` works alone.
        if !lib_dir.join("libmeetingrecord_macos.a").exists() {
            let status = Command::new(root.join("scripts/build-macos.sh"))
                .current_dir(&root)
                .status()
                .expect("failed to run scripts/build-macos.sh");
            assert!(status.success(), "native build failed");
        }

        println!("cargo:rustc-link-search=native={}", lib_dir.display());
        println!("cargo:rustc-link-lib=static=meetingrecord_macos");

        for framework in [
            "CoreAudio",
            "AVFoundation",
            "AudioToolbox",
            "Foundation",
            "AppKit",
            "ApplicationServices",
        ] {
            println!("cargo:rustc-link-lib=framework={framework}");
        }

        // The Swift runtime is not part of the static archive.
        let toolchain = Command::new("xcode-select")
            .arg("-p")
            .output()
            .map(|o| String::from_utf8_lossy(&o.stdout).trim().to_string())
            .unwrap_or_else(|_| "/Applications/Xcode.app/Contents/Developer".into());
        println!(
            "cargo:rustc-link-search=native={toolchain}/Toolchains/XcodeDefault.xctoolchain/usr/lib/swift/macosx"
        );
        println!("cargo:rustc-link-lib=dylib=swiftCore");
    }

    #[cfg(windows)]
    {
        Build::new()
            .cpp(true)
            .std("c++17")
            .include(root.join("native/include"))
            .file(root.join("native/windows/ProcessLoopback.cpp"))
            .file(root.join("native/windows/MeetingDetector.cpp"))
            .compile("meetingrecord_win");

        for lib in ["ole32", "mmdevapi", "user32"] {
            println!("cargo:rustc-link-lib=dylib={lib}");
        }
    }

    println!("cargo:rerun-if-changed=../native");
}
