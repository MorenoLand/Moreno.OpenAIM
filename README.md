# Cross-platform AIM client

OpenAIM rebuilds AOL Instant Messenger 4.7.2480 for Windows, Linux, and macOS, using OSCAR for messaging and a separate statically linked executable for each platform and architecture. The planned public repository is `MorenoLand/Moreno.OpenAIM`.

## Build

Requires CMake 3.24+, a C++20 compiler, and a static Qt 6.8+ installation with Core, Gui, Network, the platform plugin, and GIF/ICO image plugins. Release builds use UPX when it is installed; pass `-DAIM_ENABLE_UPX=OFF` to skip packing. The repository includes the original AIM artwork resources referenced by the UI.

```powershell
cmake -S . -B build -DAIM_QT_PREFIX="C:/path/to/static/Qt"
cmake --build build --config Release
```

For Linux or macOS, set `AIM_QT_PREFIX` to that platform's static Qt installation before configuring. The executable is written to `bin/OpenAIM` (`bin/OpenAIM.exe` on Windows).

The MIT license covers OpenAIM source code. The included AIM artwork remains the property of its respective rights holders and is not licensed under MIT; see [NOTICE.md](NOTICE.md). Qt components retain their own licenses. Before distributing a statically linked Qt binary, review [Qt's open-source license obligations](https://www.qt.io/development/open-source-lgpl-obligations), including its static-linking requirements.
