# Magnite LLM — CMake Build Process & Toolchain Architecture

This document provides a comprehensive technical guide to the CMake build system used for the Magnite desktop client. It covers CMake configuration, compiler flags, Qt 6 code-generation tooling (AUTOMOC, AUTORCC, AUTOUIC), source dependency management, artifact output layout, and execution verification.

---

## 1. Build System Overview

The desktop client is structured as a native C++20 application with a modern, target-oriented CMake configuration defined in [`app/CMakeLists.txt`](../../app/CMakeLists.txt).

```text
Source Tree:                           Build Output Tree:
<repo>/                                <repo>/
├── app/                               ├── build/
│   ├── CMakeLists.txt                 │   ├── CMakeCache.txt
│   ├── resources.qrc                  │   ├── build.ninja / Makefile
│   └── src/                           │   ├── magnite-app_autogen/
│       ├── main.cpp                   │   │   ├── moc_*.cpp (Generated from Q_OBJECT)
│       ├── mainwindow.cpp             │   │   └── mocs_compilation.cpp
│       ├── ...                        │   ├── qrc_resources.cpp (Compiled assets)
├── backend/                           │   └── magnite-app (Binary executable)
└── .venv/                             
```

### Key Build Invariants:
1. **Target Directory Constraint**: The resulting executable **must** reside at `<repo>/build/magnite-app`. The C++ application dynamically resolves its companion Python backend and virtual environment using `QCoreApplication::applicationDirPath() + "/../"`.
2. **Explicit Source Registration**: All C++ implementation files must be explicitly declared in `MAGNITE_SOURCES`. Globbing (`file(GLOB ...)`) is intentionally prohibited to ensure reliable dependency tracking and deterministic builds.
3. **Automated Qt Processing**: CMake coordinates with Qt's build tools to handle reflection generation (MOC) and virtual filesystem asset embedding (RCC) at compile time.

---

## 2. Line-by-Line Breakdown of `app/CMakeLists.txt`

Below is the complete CMake specification annotated with engineering rationale:

```cmake
1: cmake_minimum_required(VERSION 3.20)
2: project(magnite-app VERSION 0.1.0 LANGUAGES CXX)
3: 
4: set(CMAKE_CXX_STANDARD 20)
5: set(CMAKE_CXX_STANDARD_REQUIRED ON)
6: set(CMAKE_AUTOMOC ON)
7: set(CMAKE_AUTORCC ON)
8: set(CMAKE_AUTOUIC ON)
9: 
10: if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
11:     add_compile_options(-Wall -Wextra)
12: endif()
13: 
14: find_package(Qt6 REQUIRED COMPONENTS Widgets Network Svg)
15: 
16: set(MAGNITE_SOURCES
17:     src/testing/uicheck.cpp
18:     src/testing/uicheck.h
19:     src/main.cpp
20:     src/mainwindow.cpp
21:     src/theme.cpp
22:     src/backend_manager.cpp
23:     src/backend_client.cpp
24:     src/panels/basepanel.cpp
25:     src/panels/modelspanel.cpp
26:     src/panels/modelcard.cpp
27:     src/panels/adaptercard.cpp
28:     src/widgets/flowlayout.cpp
29:     src/widgets/combobox.cpp
30:     src/widgets/combobox.h
31:     src/widgets/clickfeedback.cpp
32:     src/widgets/clickfeedback.h
33:     src/widgets/brandmark.cpp
34:     src/widgets/brandmark.h
35:     src/widgets/starfield.cpp
36:     src/widgets/starfield.h
37:     src/widgets/overviewtile.cpp
38:     src/widgets/overviewtile.h
39:     src/widgets/confirmation.h
40:     src/widgets/surface.h
41:     src/panels/trainingpanel.cpp
42:     src/panels/ragpanel.cpp
43:     src/panels/ragdoccard.cpp
44:     src/panels/adapterspanel.cpp
45:     src/panels/chatpanel.cpp
46:     src/panels/chatbubble.cpp
47:     src/panels/settingspanel.cpp
48:     resources.qrc
49: )
50: 
51: add_executable(magnite-app ${MAGNITE_SOURCES})
52: 
53: target_include_directories(magnite-app PRIVATE src)
54: 
55: target_link_libraries(magnite-app PRIVATE
56:     Qt6::Widgets
57:     Qt6::Network
58:     Qt6::Svg
59: )
```

### Detailed Rationale by Section:

#### 1. Minimum Version & Project Setup (Lines 1–2)
```cmake
cmake_minimum_required(VERSION 3.20)
project(magnite-app VERSION 0.1.0 LANGUAGES CXX)
```
- Requires CMake 3.20 or newer, ensuring access to modern target properties, C++20 compiler feature detection, and robust Qt 6 integration.
- `LANGUAGES CXX` restricts the project to C++, avoiding unnecessary discovery checks for C or Fortran toolchains.

#### 2. C++20 Language Standard (Lines 4–5)
```cmake
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
```
- Enforces ISO C++20 without decaying to older standards (`CXX_STANDARD_REQUIRED ON`).
- C++20 features actively utilized in the codebase:
  - Designated initializers (`struct Token { QString name; QString value; };`).
  - Structured binding declarations (`const auto &[name, panel] : panels`).
  - Standard library improvements (`<algorithm>`, `<memory>`, `std::make_shared`).
  - Modern lambda capture syntax (`[this, dir = QString(argv[2])]`).

#### 3. Qt Automation Flags (Lines 6–8)
```cmake
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)
```
These settings enable CMake's built-in Qt code generators:
- **`CMAKE_AUTOMOC ON` (Meta-Object Compiler)**:
  - Scans all header files listed in `MAGNITE_SOURCES` or included by source files.
  - Detects classes declaring `Q_OBJECT` or `Q_GADGET`.
  - Automatically runs Qt's `moc` tool to generate metadata, signals, and slot dispatch tables (`moc_<filename>.cpp`).
  - Aggregates them into `magnite-app_autogen/mocs_compilation.cpp`, eliminating manual `qt6_wrap_cpp` boilerplate.
- **`CMAKE_AUTORCC ON` (Resource Compiler)**:
  - Detects `resources.qrc` in `MAGNITE_SOURCES`.
  - Invokes Qt's `rcc` tool to convert icons, fonts, stylesheet templates, and animated assets into binary byte arrays compiled directly into the binary.
- **`CMAKE_AUTOUIC ON` (User Interface Compiler)**:
  - Active for forward-compatibility. (Magnite constructs its entire UI programmatically in pure C++, so no `.ui` XML files exist).

#### 4. Compiler Warnings & Diagnostic Flags (Lines 10–12)
```cmake
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-Wall -Wextra)
endif()
```
- Activates comprehensive compiler warnings under GCC and Clang (`-Wall -Wextra`) to enforce strict type checking, unused variable elimination, and warning-free builds.

#### 5. Package Discovery (Line 14)
```cmake
find_package(Qt6 REQUIRED COMPONENTS Widgets Network Svg)
```
- Locates the Qt 6 SDK installation on the host system.
- Requires three essential modules:
  - **`Widgets` (`Qt6::Widgets`)**: Core desktop UI infrastructure (`QApplication`, `QMainWindow`, `QWidget`, `QLayout`, `QPushButton`, `QComboBox`, `QScrollArea`, `QStackedWidget`, `QGraphicsOpacityEffect`, `QStyle`).
  - **`Network` (`Qt6::Network`)**: Asynchronous HTTP client and IPC networking (`QNetworkAccessManager`, `QNetworkRequest`, `QNetworkReply`, `QTcpServer`, `QTcpSocket`, `QHostAddress`).
  - **`Svg` (`Qt6::Svg`)**: High-fidelity vector rendering for Lucide line icons and brand assets (`QSvgRenderer`, `QSvgGenerator`).

#### 6. Source Manifest (`MAGNITE_SOURCES`, Lines 16–49)
- Explicitly enumerates every source file, header, and resource bundle in the project.
- Headers containing custom widget classes (e.g., `brandmark.h`, `starfield.h`, `overviewtile.h`, `combobox.h`, `clickfeedback.h`) are included so `AUTOMOC` monitors them for changes and recompiles their MOC output immediately upon edit.
- Includes `resources.qrc` so `AUTORCC` attaches the embedded assets directly to the target.

#### 7. Target Creation, Includes & Linkage (Lines 51–59)
```cmake
add_executable(magnite-app ${MAGNITE_SOURCES})
target_include_directories(magnite-app PRIVATE src)
target_link_libraries(magnite-app PRIVATE
    Qt6::Widgets
    Qt6::Network
    Qt6::Svg
)
```
- `add_executable`: Defines the final binary target `magnite-app`.
- `target_include_directories`: Adds `src/` to the private include search paths. This enables clean, relative include statements throughout the code (e.g. `#include "theme.h"` or `#include "panels/modelspanel.h"` without requiring absolute repository roots).
- `target_link_libraries`: Links the required Qt 6 shared libraries and transitively pulls in their header directories and linker search paths.

---

## 3. The Qt Code Generation & Compilation Pipeline

When invoking CMake, source transformation and binary compilation follow a coordinated multi-stage pipeline:

```text
                           [app/resources.qrc]
                                    |
                                    v (Qt rcc)
[app/src/*.h] (Q_OBJECT)    [qrc_resources.cpp]     [app/src/*.cpp]
        |                           |                      |
        v (Qt moc)                  |                      |
[moc_*.cpp]                         |                      |
        \                           |                      /
         \                          |                     /
          v                         v                    v
      +----------------------------------------------------+
      |            C++ Compiler (g++ / clang++)            |
      |             Flags: -std=c++20 -Wall -Wextra        |
      +----------------------------------------------------+
                                    |
                                    v Object Files (.o)
      +----------------------------------------------------+
      |                 Linker (ld / lld)                  |
      |         Links: Qt6::Widgets, Network, Svg          |
      +----------------------------------------------------+
                                    |
                                    v
                         [build/magnite-app]
```

### 3.1 MOC Generation (`magnite-app_autogen`)
1. Before compiling any `.cpp` file, the build generator runs the `magnite-app_autogen` custom target.
2. CMake inspects header timestamps and parses files for the `Q_OBJECT` macro.
3. For headers declaring signals or slots (`mainwindow.h`, `backend_manager.h`, `backend_client.h`, `basepanel.h`, panels, and interactive cards), `moc` emits C++ reflection tables and signal emission stubs.
4. These are compiled into `build/magnite-app_autogen/mocs_compilation.o`.

### 3.2 Asset Bundling via RCC
1. Qt's Resource Compiler (`rcc`) reads `app/resources.qrc`.
2. It compresses all referenced static files:
   - Global stylesheet: `resources/theme/console.qss` (aliased as `theme/theme.qss`).
   - Bundled fonts: `SpaceGrotesk-*.ttf`, `DMSans-*.ttf`.
   - 17 Lucide SVG icons: `database.svg`, `message-circle.svg`, `box.svg`, etc.
   - Brand assets: `wordmark.svg`, `symbol.svg`, `animated-symbol.gif`.
3. It generates `build/qrc_resources.cpp`, a C++ file containing byte arrays registered with Qt's virtual resource tree at program startup. At runtime, the C++ code accesses these assets via the `:/` prefix.

---

## 4. Build Prerequisites & Toolchain Setup

### 4.1 System Dependencies

#### Linux (Debian / Ubuntu / Pop!_OS)
```bash
sudo apt-get update
sudo apt-get install -y \
    build-essential \
    cmake \
    ninja-build \
    qt6-base-dev \
    libqt6svg6-dev \
    libgl1-mesa-dev
```

#### macOS (Homebrew)
```bash
brew install cmake ninja qt@6
brew link qt@6
```

#### Windows (vcpkg / MSYS2)
Install Visual Studio 2022 (with C++ Desktop workload) and install Qt 6 via the official Qt Online Installer or vcpkg (`vcpkg install qtbase[widgets,network]:x64-windows qtsvg:x64-windows`).

---

## 5. Build Instructions & Developer Workflows

### 5.1 Standard Build Procedure
The build is executed from the repository root:

```bash
# 1. Configure the build directory using Ninja (or Make)
cmake -S app -B build -G Ninja

# 2. Compile using all available CPU cores
cmake --build build -j$(nproc)
```

*(Note: If Ninja is not installed, omit `-G Ninja` to use standard Unix Makefiles: `cmake --build build -j`).*

### 5.2 Build Types (Debug vs. Release)

#### Debug Build (Symbols, no optimization, assertion checks):
```bash
cmake -S app -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
```

#### Release Build (Full optimizations, minimal binary size):
```bash
cmake -S app -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

### 5.3 Clean & Rebuild
```bash
# Clean compiled artifacts without removing CMake configuration
cmake --build build --target clean

# Full clean rebuild from scratch
rm -rf build
cmake -S app -B build
cmake --build build -j
```

---

## 6. Verification & Automated Quality Assurance

Once compiled, `build/magnite-app` provides built-in validation flags that test the binary at different integration tiers:

### 6.1 Theme & Resource Audit (`--theme-check`)
Validates that all design tokens, font families, Lucide SVG assets, and runtime stylesheets compile and pass WCAG 4.5:1 contrast requirements without opening a GUI window.
```bash
./build/magnite-app --theme-check
```
*Expected output:* `[theme-check] 0 failure(s)`.

### 6.2 UI Layout & Reflow Verification (`--ui-check`)
Spawns an in-process mock loopback server (`QTcpServer`), tests responsive layout reflows (at 848px and 1104px), keyboard navigation, and button focus without downloading ML models.
```bash
./build/magnite-app --ui-check
```
*Optional screenshot capture during UI check:*
```bash
./build/magnite-app --ui-check /tmp/ui-captures
```
*Expected output:* `[ui-check] PASS (0 failures)`.

### 6.3 End-to-End Desktop Selftest (`--selftest`)
Launches the real Python backend, verifies IPC discovery, tests model registry listings, fine-tuning dispatch, adapter hot-switching, and RAG document queries.
```bash
./build/magnite-app --selftest
```
*Feature-specific e2e flags:*
```bash
MAGNITE_SELFTEST_PULL=1       ./build/magnite-app --selftest   # Model pull from Hub
MAGNITE_SELFTEST_INFERENCE=1  ./build/magnite-app --selftest   # In-process llama.cpp inference
MAGNITE_SELFTEST_TRAINING=1   ./build/magnite-app --selftest   # LoRA fine-tuning job
MAGNITE_SELFTEST_ADAPTER=1    ./build/magnite-app --selftest   # Adapter train + hot-swap
MAGNITE_SELFTEST_RAG=1        ./build/magnite-app --selftest   # Vector ingestion and query
```
*Expected output:* `[selftest] PASS (0 failures)`.

### 6.4 Headless Screenshot Capture (`--screenshot`)
Renders all 6 panels in-process and captures pixel-accurate `.png` screenshots to a specified directory (safe for Wayland and offscreen environments).
```bash
QT_QPA_PLATFORM=offscreen ./build/magnite-app --screenshot /tmp/magnite-panels
```

---

## 7. Continuous Integration Pipeline (CI)

The GitHub Actions workflow defined in [`.github/workflows/verify.yml`](../../.github/workflows/verify.yml) automates this build and verification process on Ubuntu 24.04:

```yaml
- name: Install native prerequisites
  run: |
    sudo apt-get update
    sudo apt-get install --no-install-recommends -y \
      build-essential cmake ninja-build qt6-base-dev libqt6svg6-dev libgl1-mesa-dev

- name: Build client
  run: |
    cmake -S app -B build -G Ninja
    cmake --build build -j2

- name: Theme audit
  run: ./build/magnite-app --theme-check

- name: UI interaction and layout checks
  run: ./build/magnite-app --ui-check

- name: Desktop and IPC selftest
  run: ./build/magnite-app --selftest
```
