<!-- Copyright (C) 2026-2027 Ziad Mohsin. All Rights Reserved. -->
# ⚙️ How MountX Builds and Executes with `mountx-native`

This document provides a comprehensive technical overview of how **MountX** evolved from a reference interpreter into an ultra-fast, standalone native executable engine (**`mountx-native`** on Linux, **`mountx.exe`** on Windows, and **`mountx.wasm`** on WebAssembly).

---

## 📌 Executive Summary

`mountx-native` is a **standalone, high-performance native C binary** that executes MountX (`.mx`) applications at native speeds without requiring Python, a virtual machine, or any external runtimes.

It is formed by combining four components into a single static executable:
1. **The C Substrate Runtime (`gate2/g1/mini.c`)**: High-performance value representation, arena allocators, string interning, and native system bindings.
2. **The Transpiled Self-Hosting Engine (`gate2/g1/interp.emitted.c`)**: The complete MountX interpreter logic, written in MountX itself and transpiled into C99.
3. **The AOT Embedded Standard Library (`gate2/g1/stdlib_aot.c`)**: All 43 official standard library modules pre-compiled and embedded directly inside the binary.
4. **Embedded Native Subsystems**: Embedded SQLite 3 database engine (`sqlite3.c`) and live Berkeley/Winsock TCP networking.

---

## 🏛️ The Three-Phase Architectural Journey

To understand how `mountx-native` was achieved, consider MountX's three evolutionary phases:

```
[Phase 1: Python Host Reference]
       main.py -> lexer.py -> parser.py -> interpreter.py
                            │
                            ▼
[Phase 2: Self-Hosting in MountX]
       selfhost/lexer.mx -> selfhost/parser.mx -> selfhost/interp.mx
                            │
                            ▼
[Phase 3: The C Substrate & AOT Transpilation]
       interp.mx  ──(transpiled via compiler.mx)──>  interp.emitted.c
                                                            │
    mini.c (C Substrate) + stdlib_aot.c + sqlite3.c ────────┘
                            │
                     [GCC / MinGW / Clang]
                            ▼
           mountx-native (Linux) / mountx.exe (Windows)
```

### Phase 1 — Python Host Reference
MountX began as an interpreted language in Python with recursive descent parsing, dynamic environment chaining, lexical scoping, and object-oriented features.

### Phase 2 — 100% Self-Hosting Parity
The entire language frontend was rewritten in MountX itself:
- `selfhost/lexer.mx`: Tokenizes MountX code inside MountX.
- `selfhost/parser.mx`: Generates AST dictionaries inside MountX.
- `selfhost/interp.mx`: Complete tree-walking evaluator inside MountX.
Every self-hosted component achieved byte-for-byte differential parity against the Python host across 65+ guest programs and 343 test assertions.

### Phase 3 — Transpilation to C & Substrate Fusion
To eliminate the 400x overhead of interpreting an interpreter:
- A MountX-to-C transpiler (`compiler.mx`) translated `interp.mx` into clean C99 code (`interp.emitted.c`).
- A specialized C runtime floor (`mini.c`) was engineered to provide the memory, execution, and native systems substrate.
- The two halves were compiled together using `-DMX_SUBSTRATE -DMX_BOTH`, creating `mountx-native`.

---

## 🔬 Core Engineering Innovations in `mountx-native`

The speed of `mountx-native` (5x to 20x faster than the Python interpreter) is driven by specific low-level C optimizations:

### 1. Tagged Value Representation (`MxValue`)
In `gate2/g1/mini.c`, every value in MountX is represented by a compact, cache-friendly tagged struct:
```c
typedef enum {
    MX_NULL = 0,
    MX_BOOL,
    MX_INT,
    MX_NUM,
    MX_STR,
    MX_ARR,
    MX_OBJ,
    MX_INST,
    MX_GENINST,
    MX_FUNC,
    MX_MODULE,
    MX_SUPER
} MxTag;

typedef struct MxValue {
    MxTag tag;
    union {
        int64_t i;
        double f;
        const char *s;
        struct MxArray *a;
        struct MxDict *d;
        struct MxInstance *inst;
        /* ... */
    } as;
} MxValue;
```
- **Integer Fast Path**: Arithmetic operations (`+`, `-`, `*`, `%`, `==`, `<`, `>`) on integers bypass heap allocation and execute directly as CPU register instructions.
- **Strict Division Invariant**: In MountX, `/` always yields `MX_NUM` (floating point), while integer truncation is handled explicitly by `idiv`.

### 2. Small Binding Buffer (`MxEnv`)
Variable lookups in tree-walking interpreters typically incur heavy malloc/free churn per scope. `mountx-native` eliminates this:
```c
struct MxEnv {
    MxEnv *parent;
    Binding *b;
    uint32_t n;
    uint32_t cap;
    Binding small_b[8]; // Inline stack buffer for up to 8 variables
};
```
For blocks, loops, and small functions with 8 or fewer variables, **zero heap allocations** occur.

### 3. String Interning & Symbol Cache (`mx_intern`)
Identifiers, method names, and dictionary keys are interned into canonical pointers via a direct-mapped cache (`g_ptr_cache` with 4,096 entries).
- Variable lookup (`env_get`, `env_set`) and dictionary lookups (`mx_dict_get`) compare interned pointers directly via `ptr1 == ptr2` in **O(1)** time, eliminating repeated `strcmp` string scans.

### 4. AOT Embedded Standard Library (`stdlib_aot.c`)
Unlike traditional interpreters that load standard library files from the disk at startup:
- All 43 modules (covering `core`, `data`, `structs`, `text`, `math`, `system`, `net`, `http_server`, `orm`) are parsed ahead of time into a static C array (`stdlib_aot.c`).
- When a user writes `importx http_server`, the AST is instantiated instantaneously from binary memory with zero filesystem IO.

### 5. Native SQLite3 & Live TCP Sockets
- SQLite 3 is compiled directly into the binary from `gate2/g1/sqlite3.c`, enabling embedded SQL queries (`db_open`, `db_exec`, `db_query`) at C speeds.
- Berkeley TCP Sockets (`tcp_listen`, `tcp_accept`, `tcp_recv`, `tcp_send`) operate directly through OS kernel syscalls, powering `http_server.mx` without third-party web servers.

---

## 🛠️ The Multi-Target Build Pipeline

MountX can be compiled for three targets using simple build scripts:

### 1. Linux x86-64 Native (`./mountx-native`)
Built directly with GCC:
```bash
gcc -std=c99 -O2 -Wall -Wextra \
    -DMX_SUBSTRATE -DMX_BOTH \
    -I gate2/g1 \
    gate2/g1/mini.c \
    gate2/g1/interp.emitted.c \
    gate2/g1/stdlib_aot.c \
    gate2/g1/sqlite3.c \
    -lm -lpthread -ldl -o mountx-native
```
Produces a single, self-contained binary (~4.9 MB) that runs anywhere on Linux.

### 2. Windows x86-64 Native (`mountx.exe`)
Cross-compiled from Linux using MinGW-w64 or Zig (`build_win.sh`):
```bash
x86_64-w64-mingw32-gcc -std=c99 -O2 -Wall -Wextra \
    -DMX_SUBSTRATE -DMX_BOTH \
    -I gate2/g1 \
    gate2/g1/mini.c \
    gate2/g1/interp.emitted.c \
    gate2/g1/stdlib_aot.c \
    tmp/sqlite3_win.o \
    -lws2_32 -o gate2/g1/mountx.exe
```
Links `ws2_32.lib` for native Windows socket networking.

### 3. WebAssembly / In-Browser Runtime (`web/mountx.wasm`)
Compiled using Emscripten (`tools/build_wasm.sh`):
```bash
emcc -std=c99 -O2 \
    -DMX_SUBSTRATE -DMX_BOTH \
    -s WASM=1 \
    -s ALLOW_MEMORY_GROWTH=1 \
    -s SUPPORT_LONGJMP=wasm \
    -s EXPORTED_FUNCTIONS="['_mountx_run','_free','_malloc']" \
    -s EXPORTED_RUNTIME_METHODS="['ccall','cwrap','UTF8ToString','stringToUTF8']" \
    -I gate2/g1 \
    gate2/g1/mini.c gate2/g1/interp.emitted.c gate2/g1/stdlib_aot.c gate2/g1/sqlite3.c \
    -o web/mountx.js
```
Produces `web/mountx.wasm` (10.8 MB) and `web/mountx.js` (90 KB), enabling direct, client-side execution in any modern web browser without a backend.

---

## 🚀 How Execution Works at Runtime

When you run a command such as:
```bash
./mountx-native my_program.mx
```

1. **Bootstrap Entry (`main` in `interp.emitted.c`)**:
   - Initializes global safety nets (signals, stack limits, memory arena).
   - Sets up `g_globals` environment and interns native symbol names.
2. **Reading & Ingestion**:
   - Reads `my_program.mx` from disk via POSIX file IO.
3. **AST Construction**:
   - `mini.c`'s C-level lexer and parser convert the source text into an in-memory `MxNode` AST in microseconds.
4. **Execution Loop**:
   - The emitted evaluation functions execute the AST nodes sequentially.
   - Builtin functions (`printx`, `readx`, `time_now_ms`) dispatch directly to native C functions.
   - Database and socket calls dispatch directly to SQLite and kernel socket descriptors.
5. **Clean Exit**:
   - Memory arena is released and process terminates with the exit code.

---

## 📦 Developer Experience & Project Distribution

With `mountx-native` established as the core runtime, modern tooling completes the ecosystem:
- **`mxpm` Package Manager**:
  - Initializes projects: `mxpm init <app-name>`
  - Installs dependencies into `libs/`: `mxpm add <package>`
  - Runs scripts: `mxpm run start`
  - Packages redistributable libraries: `mxpm pack`
- **Workbench Studio (`./workbench`)**:
  - Full-featured GTK 4 + Libadwaita dark studio with code editor, telemetry, and live runner.
- **VS Code Extension**:
  - Syntax highlighting and code snippets (`tools/vscode/mountx-1.0.0.vsix`).
- **WebAssembly Playground (`web/index.html`)**:
  - Live in-browser coding playground with zero installation friction.
