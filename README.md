# Cross-platform AIM client

OpenAIM is a C++20/Qt reimplementation of AOL Instant Messenger 4.7.2480 for OSCAR-compatible servers, preserving the original interface resources. It targets a single statically linked executable per platform and architecture.

Windows is the tested platform. Linux and macOS ports are in progress; native preferences and audio features are not yet complete on those platforms. The client is under development and does not claim complete AIM feature or visual parity.

## Build

Requires CMake 3.24+, a C++20 compiler, and a static Qt 6.8+ installation with Core, Gui, Network, the platform plugin, and GIF/ICO image plugins. Release builds use UPX when it is installed; pass `-DAIM_ENABLE_UPX=OFF` to skip packing. The repository includes the original AIM artwork resources referenced by the UI.

```powershell
cmake -S . -B build -DAIM_QT_PREFIX="C:/path/to/static/Qt"
cmake --build build --config Release
```

For Linux or macOS, set `AIM_QT_PREFIX` to that platform's static Qt installation before configuring. The executable is written to `bin/OpenAIM` (`bin/OpenAIM.exe` on Windows).

With a single-configuration generator, also configure `-DCMAKE_BUILD_TYPE=Release`.

## Connect and run

Set your OSCAR server host and port in Setup > Connection. The original AIM service is not provided by this project. The client has been exercised against [Open OSCAR Server](https://github.com/mk6i/open-oscar-server).

Separate account and preference stores can be selected with profiles:

```powershell
.\bin\OpenAIM.exe --profile=test1
.\bin\OpenAIM.exe --profile=test2
```

Without `--profile`, the normal OpenAIM settings store is used.

## Validation

```powershell
cmake -S . -B build-selftest -DAIM_QT_PREFIX="C:/path/to/static/Qt" -DAIM_BUILD_SELFTEST=ON
cmake --build build-selftest --config Release --target openaim_selftest openaim_atecolortest
.\build-selftest\bin\openaim_atecolortest.exe
.\build-selftest\bin\openaim_selftest.exe --talk
```

The full network self-test requires your own test server and the dedicated accounts `openaimtest1` and `openaimtest2`. Supply a local UTF-8 file with one `screenname<TAB>password` entry per line; never commit it. It modifies only those test accounts' messaging, chat, warning, and roster state.

```powershell
$env:OPENAIM_TEST_HOST="your-server.example"
.\build-selftest\bin\openaim_selftest.exe "C:/path/to/test-accounts.local.txt"
```

The current live suite has one known failure: its group cleanup is blocked by the roster deletion safety guard. That guard is retained until the server's deletion semantics can be relied on. Buddy icons, mail, file transfer/sharing, some menu actions, and portions of Talk artwork remain incomplete. Real AIM-peer interoperability and Linux/macOS operation are not fully validated.

## Licenses

The MIT license covers OpenAIM source code. The included AIM artwork remains the property of its respective rights holders and is not licensed under MIT; see [NOTICE.md](NOTICE.md). Qt components retain their own licenses. Before distributing a statically linked Qt binary, review [Qt's open-source license obligations](https://www.qt.io/development/open-source-lgpl-obligations), including its static-linking requirements.
