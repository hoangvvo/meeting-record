{
  "targets": [
    {
      "target_name": "meeting_record",
      "sources": ["src/native/binding.cc"],
      "include_dirs": [
        "<!@(node -p \"require('node-addon-api').include\")",
        "../native/include"
      ],
      "defines": ["NAPI_DISABLE_CPP_EXCEPTIONS", "NAPI_VERSION=8"],
      "cflags_cc": ["-std=c++17"],
      "conditions": [
        ["OS=='linux'", {
          "sources": [
            "../native/common/meeting-catalog.c",
            "../native/common/tracks.c",
            "../native/linux/Capture.cpp",
            "../native/linux/MeetingDetector.cpp",
            "../native/linux/Accessibility.cpp"
          ],
          "include_dirs": ["../native/common", "<!@(pkg-config --cflags-only-I libpulse atspi-2 gobject-2.0 | sed s/-I//g)"],
          "libraries": ["<!@(pkg-config --libs libpulse atspi-2 gobject-2.0)", "-lpthread"],
          "cflags_cc!": ["-fno-exceptions"],
          "cflags_cc": ["-std=c++17", "-fexceptions"]
        }],
        ["OS=='mac'", {
          "variables": {
            "xcode_path": "<!(xcode-select -p)"
          },
          # The Swift core is built by ../scripts/build-macos.sh; this only links it.
          # Run `npm run build:native` first.
          "libraries": [
            "<(module_root_dir)/../native/build/libmeetingrecord_macos.a",
            "-framework CoreAudio",
            "-framework AVFoundation",
            "-framework AudioToolbox",
            "-framework Foundation",
            "-framework AppKit",
            "-framework ApplicationServices",
            "-L<(xcode_path)/Toolchains/XcodeDefault.xctoolchain/usr/lib/swift/macosx",
            "-lswiftCore"
          ],
          "xcode_settings": {
            "MACOSX_DEPLOYMENT_TARGET": "14.2",
            "CLANG_CXX_LIBRARY": "libc++",
            "GCC_ENABLE_CPP_EXCEPTIONS": "NO"
          }
        }],
        ["OS=='win'", {
          "sources": [
            "../native/common/meeting-catalog.c",
            "../native/common/tracks.c",
            "../native/windows/ProcessLoopback.cpp",
            "../native/windows/MeetingDetector.cpp"
          ],
          "include_dirs": ["../native/common"],
          "libraries": ["advapi32.lib", "ole32.lib", "oleaut32.lib", "mmdevapi.lib", "user32.lib"],
          "msvs_settings": {
            "VCCLCompilerTool": { "ExceptionHandling": 1, "AdditionalOptions": ["/std:c++17"] }
          }
        }]
      ]
    }
  ]
}
