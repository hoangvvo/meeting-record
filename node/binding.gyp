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
            "../native/windows/ProcessLoopback.cpp",
            "../native/windows/MeetingDetector.cpp"
          ],
          "libraries": ["ole32.lib", "mmdevapi.lib", "user32.lib"],
          "msvs_settings": {
            "VCCLCompilerTool": { "ExceptionHandling": 1, "AdditionalOptions": ["/std:c++17"] }
          }
        }]
      ]
    }
  ]
}
