See https://github.com/kensuke/How-to-Miracast-on-AOSP/wiki

Everything you want..


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
│       └── tuna
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
│   │           ├── ACodec.cpp
│   │           └── wifi-display
│   │               ├── ANetworkSession.cpp
│   │               ├── sink
│   │               │   └── TunnelRenderer.cpp
│   │               └── source
│   │                   └── WifiDisplaySource.cpp
│   ├── base
│   │   └── services
│   │       └── java
│   │           └── com
│   │               └── android
│   │                   └── server
│   │                       └── display
│   │                           └── WifiDisplayController.java
│   └── native
│       └── libs
│           └── gui
│               └── SurfaceTexture.cpp
└── packages
    └── apps
        ├─ Mira4U // JNI Sink, other util app
        └─ WFD    // wfd cmd test app
```
