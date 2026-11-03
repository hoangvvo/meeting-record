{
  "targets": [
    {
      "target_name": "meeting_record",
      "sources": ["src/native/binding.cc"],
      "include_dirs": [
        "<!@(node -p \"require('node-addon-api').include\")",
        "../native/include",
        "../native/third_party/speexdsp",
        "../native/third_party/speexdsp/include"
      ],
      "defines": ["NAPI_DISABLE_CPP_EXCEPTIONS", "NAPI_VERSION=8", "HAVE_CONFIG_H"],
      "cflags_cc": ["-std=c++17"],
      "conditions": [
        ["OS=='linux' or OS=='win'", {
          "sources": [
            "../native/common/meeting-catalog.c",
            "../native/common/tracks.c",
            "../native/common/voice-processing.cpp",
            "../native/common/pcm-mixer.cpp",
            "../native/third_party/speexdsp/libspeexdsp/mdf.c",
            "../native/third_party/speexdsp/libspeexdsp/preprocess.c",
            "../native/third_party/speexdsp/libspeexdsp/filterbank.c",
            "../native/third_party/speexdsp/libspeexdsp/fftwrap.c",
            "../native/third_party/speexdsp/libspeexdsp/kiss_fft.c",
            "../native/third_party/speexdsp/libspeexdsp/kiss_fftr.c"
          ],
          "include_dirs": ["../native/common"]
        }],
        ["OS=='linux'", {
          "sources": [
            "../native/linux/Capture.cpp",
            "../native/linux/MeetingDetector.cpp",
            "../native/linux/Accessibility.cpp"
          ],
          "include_dirs": ["<!@(pkg-config --cflags-only-I libpulse atspi-2 gobject-2.0 | sed s/-I//g)"],
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
            "../native/windows/ProcessLoopback.cpp",
            "../native/windows/MeetingDetector.cpp"
          ],
          "libraries": ["advapi32.lib", "avrt.lib", "ole32.lib", "oleaut32.lib", "mmdevapi.lib", "user32.lib"],
          "msvs_settings": {
            "VCCLCompilerTool": { "ExceptionHandling": 1, "AdditionalOptions": ["/std:c++17"] }
          }
        }]
      ]
    }
  ]
}
