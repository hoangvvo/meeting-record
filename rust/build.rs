//! Builds and links the native backend bundled with the crate.

use std::{
    env, fs,
    path::{Path, PathBuf},
    process::Command,
};

fn main() {
    let manifest = PathBuf::from(env::var_os("CARGO_MANIFEST_DIR").unwrap());
    let native = native_root(&manifest);
    println!("cargo:rerun-if-env-changed=DOCS_RS");
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
    let workspace = manifest
        .parent()
        .expect("the development crate must be inside the workspace")
        .join("native");
    if workspace.is_dir() {
        return workspace;
    }

    let packaged = manifest.join("native");
    if packaged.is_dir() {
        return packaged;
    }

    panic!(
        "native sources are missing from both {} and {}",
        packaged.display(),
        workspace.display()
    )
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
    for source in &sources {
        println!("cargo:rerun-if-changed={}", source.display());
    }
    for header in [
        "include/meeting-record.h",
        "include/meeting-record-detect.h",
        "include/meeting-record-events.h",
        "macos/Bridging.h",
    ] {
        println!("cargo:rerun-if-changed={}", native.join(header).display());
    }

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
        "-import-objc-header",
    ]);
    swift.arg(native.join("macos/Bridging.h"));
    swift.arg("-o").arg(&output).args(&sources);
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

    let sdk = Command::new("xcrun")
        .args(["--sdk", "macosx", "--show-sdk-path"])
        .output()
        .expect("failed to locate the macOS SDK through xcrun");
    assert!(sdk.status.success(), "xcrun --show-sdk-path failed");
    let sdk = String::from_utf8(sdk.stdout).expect("xcrun returned a non-UTF-8 SDK path");
    let swift_libraries = Path::new(sdk.trim()).join("usr/lib/swift");
    println!(
        "cargo:rustc-link-search=native={}",
        swift_libraries.display()
    );
    println!("cargo:rustc-link-lib=dylib=swiftCore");
    build_common(native);
}

fn build_windows(native: &Path) {
    for source in [
        "include/meeting-record.h",
        "include/meeting-record-detect.h",
        "include/meeting-record-events.h",
        "windows/ProcessLoopback.cpp",
        "windows/AudioAppProcess.h",
        "windows/CapturePump.h",
        "windows/MeetingDetector.cpp",
        "windows/TimelineMixer.h",
    ] {
        println!("cargo:rerun-if-changed={}", native.join(source).display());
    }
    cc::Build::new()
        .cpp(true)
        .std("c++17")
        .include(native.join("include"))
        .include(native.join("common"))
        .file(native.join("windows/ProcessLoopback.cpp"))
        .file(native.join("windows/MeetingDetector.cpp"))
        .compile("meetingrecord_win");

    for library in ["advapi32", "ole32", "oleaut32", "mmdevapi", "user32"] {
        println!("cargo:rustc-link-lib=dylib={library}");
    }
    build_common(native);
}

/// Meeting classification and source recovery shared by both backends.
fn build_common(native: &Path) {
    for source in [
        "common/meeting-catalog.h",
        "common/meeting-catalog.c",
        "common/tracks.h",
        "common/tracks.c",
    ] {
        println!("cargo:rerun-if-changed={}", native.join(source).display());
    }
    cc::Build::new()
        .include(native.join("include"))
        .file(native.join("common/meeting-catalog.c"))
        .file(native.join("common/tracks.c"))
        .compile("meetingrecord_common");
}
