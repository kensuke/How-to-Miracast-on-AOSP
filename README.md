See https://github.com/kensuke/How-to-Miracast-on-AOSP/wiki

Everything you want..



Modified Files List
```
├── build
│   └── target
│       └── product
│           └── generic_no_telephony.mk // pre-install app setting
├── device
│   ├── asus
│   │   ├── grouper // Nexus 7 Wi-Fi
│   │   │   ├── audio_policy.conf
│   │   │   ├── device-common.mk
│   │   │   └── overlay/framewsorks/base/core/res/res/values/config.xml
│   │   └── tilapia // Nexus 7 3G
│   │        └── overlay/framewsorks/base/core/res/res/values/config.xml
│   └── samsung
│       ├── maguro // Galaxy Nexus
│       │    └── overlay/framewsorks/base/core/res/res/values/config.xml
│       └── tuna // Samsung Common
│           ├── audio
│           │   └── audio_policy.conf
│           ├── device.mk
│           └── media_codecs.xml
├── docs
│   └── images
│       └─ // some images for wiki documentation
├── frameworks
│   ├── av
│   │   └── media
│   │       └── libstagefright
│   │           ├── ACodec.cpp // Source
│   │           └── wifi-display
│   │               ├── ANetworkSession.cpp // Debug Log
│   │               ├── sink
│   │               │   ├── TunnelRenderer.cpp
│   │               │   └── WifiDisplaySink.cpp
│   │               └── source
│   │                   └── WifiDisplaySource.cpp
│   ├── base
│   │   └── services
│   │       └── java
│   │           └── com
│   │               └── android
│   │                   └── server
│   │                       └── display
│   │                           └── WifiDisplayController.java // Sink
│   └── native
│       └── libs
│           └── gui
│               └── SurfaceTexture.cpp // Sink: Screen Rotation
└── packages
    └── apps
        ├─ Mira4U // JNI Sink, other util app
        ├─ Settings/src/com/android/settings/wfd/WifiDisplaySettings.java // On/Off Switch
        └─ WFD    // wfd cmd test app
```
