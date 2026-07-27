//! Builds and links the native backend bundled with the crate.

use std::env;
use std::fs;
use std::path::{Path, PathBuf};
use std::process::Command;

fn main() {
    let manifest = PathBuf::from(env::var_os("CARGO_MANIFEST_DIR").unwrap());
    let native = native_root(&manifest);
    println!("cargo:rerun-if-changed={}", native.display());

    // docs.rs has no macOS or Windows SDK. Rustdoc does not link the externs,
    // so it can still document the safe wrapper without building a backend.
    if env::var_os("DOCS_RS").is_some() {
        return;
    }

    match env::var("CARGO_CFG_TARGET_OS").as_deref() {
        Ok("macos") => build_macos(&native),
        Ok("windows") => build_windows(&native),
        Ok(platform) => panic!("meeting-record does not support {platform}"),
        Err(error) => panic!("Cargo did not provide the target OS: {error}"),
    }
}

fn native_root(manifest: &Path) -> PathBuf {
    let packaged = manifest.join("native");
    if packaged.is_dir() {
        return packaged;
    }

    let workspace = manifest
        .parent()
        .expect("the development crate must be inside the workspace")
        .join("native");
    assert!(
        workspace.is_dir(),
        "native sources are missing from both {} and {}",
        packaged.display(),
        workspace.display()
    );
    workspace
}

fn build_macos(native: &Path) {
    let swift_target = match env::var("CARGO_CFG_TARGET_ARCH").as_deref() {
        Ok("aarch64") => "arm64-apple-macos14.2",
        Ok("x86_64") => "x86_64-apple-macos14.2",
        Ok(arch) => panic!("unsupported macOS architecture: {arch}"),
        Err(error) => panic!("Cargo did not provide the target architecture: {error}"),
    };

    let mut sources = fs::read_dir(native.join("macos"))
        .expect("failed to read bundled macOS sources")
        .map(|entry| entry.expect("failed to read a macOS source entry").path())
        .filter(|path| {
            path.extension()
                .is_some_and(|extension| extension == "swift")
        })
        .collect::<Vec<_>>();
    sources.sort();
    assert!(!sources.is_empty(), "no bundled macOS sources were found");

    let output = PathBuf::from(env::var_os("OUT_DIR").unwrap()).join("libmeetingrecord_macos.a");
    let mut swift = Command::new("xcrun");
    swift.args([
        "swiftc",
        "-O",
        "-parse-as-library",
        "-emit-library",
        "-static",
        "-target",
        swift_target,
        "-o",
    ]);
    swift.arg(&output).args(&sources);
    let status = swift
        .status()
        .expect("failed to start swiftc through xcrun");
    assert!(status.success(), "swiftc failed with {status}");

    println!(
        "cargo:rustc-link-search=native={}",
        output.parent().unwrap().display()
    );
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

    let developer_dir = Command::new("xcode-select")
        .arg("-p")
        .output()
        .expect("failed to run xcode-select");
    assert!(developer_dir.status.success(), "xcode-select -p failed");
    let developer_dir =
        String::from_utf8(developer_dir.stdout).expect("xcode-select returned a non-UTF-8 path");
    let swift_runtime = Path::new(developer_dir.trim())
        .join("Toolchains/XcodeDefault.xctoolchain/usr/lib/swift/macosx");
    println!("cargo:rustc-link-search=native={}", swift_runtime.display());
    println!("cargo:rustc-link-lib=dylib=swiftCore");
}

fn build_windows(native: &Path) {
    cc::Build::new()
        .cpp(true)
        .std("c++17")
        .include(native.join("include"))
        .file(native.join("windows/ProcessLoopback.cpp"))
        .file(native.join("windows/MeetingDetector.cpp"))
        .compile("meetingrecord_win");

    for library in ["ole32", "mmdevapi", "user32"] {
        println!("cargo:rustc-link-lib=dylib={library}");
    }
}
