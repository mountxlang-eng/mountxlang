<!-- Copyright (C) 2026-2027 Ziad Mohsin. All Rights Reserved. -->
# 🏔️ MountX Programming Language

> **A modern, expressive, high-performance programming language featuring a native C AOT-compiled substrate, built-in full-stack web and SQLite ORM engines, the official `mxpm` package manager, and client-side WebAssembly execution.**

[![Version](https://img.shields.io/badge/version-1.1.0-blue.svg)](file:///home/theyad75/Desktop/antmountxlang/host/interpreter.py)
[![Tests](https://img.shields.io/badge/tests-343%20passed%20(100%25)-brightgreen.svg)](file:///home/theyad75/Desktop/antmountxlang/host/fbtest.mx)
[![Docs](https://img.shields.io/badge/docs-60%20verified-success.svg)](file:///home/theyad75/Desktop/antmountxlang/docs_verify.py)
[![Native Runtime](https://img.shields.io/badge/native-C99%20Substrate%20(AOT)-orange.svg)](file:///home/theyad75/Desktop/antmountxlang/docs/HOW_MOUNTX_NATIVE_WORKS.md)
[![WebAssembly](https://img.shields.io/badge/webassembly-browser%20live-purple.svg)](file:///home/theyad75/Desktop/antmountxlang/web/index.html)
[![Package Manager](https://img.shields.io/badge/package%20manager-mxpm-teal.svg)](file:///home/theyad75/Desktop/antmountxlang/docs/MXPM_GUIDE.md)
[![License](https://img.shields.io/badge/license-MIT-lightgrey.svg)](LICENSE)

---
# MountX Programming Language

## 🌟 Key Highlights & Ecosystem

MountX combines the clean, expressive syntax of modern dynamic languages with the speed and zero-dependency footprint of low-level C runtimes:

* ⚡ **High-Performance Native C Substrate (`mountx-native` / `mountx.exe`)**:
  Pre-compiled C99 engine featuring tagged value layouts, arena memory allocation, string interning with $O(1)$ pointer comparison, and inline 8-variable environment frames. Executes 5x to 20x faster than pure interpreted code with sub-2ms startup time.
* 🌐 **Built-in Full-Stack Web Engine (`http_server.mx`)**:
  Native HTTP/1.1 server powered directly by live Berkeley TCP sockets (`socket.mx`). Features dynamic route matching (`/users/:id`), middleware pipelines (`app.use`), JSON body parsers, and static asset serving—no external web server required.
* 💾 **Embedded SQLite 3 & Active-Record ORM (`db.mx` & `orm.mx`)**:
  Full SQLite engine compiled directly into the binary. Includes a lightweight ORM (`Model`) supporting query builders, migrations, and parameterized queries (`find`, `where`, `save`).
* 📦 **Official Package Manager (`mxpm`)**:
  Full-featured project manager for MountX. Scaffolds projects (`mxpm init`), installs and links packages into `libs/` (`mxpm add`), runs project scripts (`mxpm run`), packages `.mxpkg` archives (`mxpm pack`), and launches the WebAssembly sandbox (`mxpm playground`).
* 🌍 **WebAssembly Target (`mountx.wasm`)**:
  Full in-browser runtime compiled via Emscripten. Run MountX code client-side in any modern web browser with zero server roundtrips.
* 🎨 **First-Class Developer Tooling**:
  Official VS Code extension with syntax highlighting and snippets (`mountx-1.0.0.vsix`), interactive REPL, and a GTK 4 / Libadwaita dark studio ([Workbench Studio](file:///home/theyad75/Desktop/antmountxlang/workbench)).

---

## 🚀 Quickstart

### 1. Run with Native High-Performance Binary
```bash
./mountx-native program.mx
```

### 2. Run with Host CLI Wrapper
```bash
./mountx program.mx         # Run any .mx source file
./mountx                    # Start the interactive REPL
./mountx -e 'printx("hi")'  # Execute an inline snippet
```

To use `mountx` globally, add it to your `PATH` or create a symlink:
```bash
ln -s "$PWD/mountx" ~/.local/bin/mountx
```

### 3. Launch In-Browser WebAssembly Playground
Test and experiment with MountX directly in your browser:
```bash
./mxpm playground
# Or using the direct launcher:
./tools/serve_web.sh
```
Opens `http://localhost:8088` with an interactive code editor and real-time execution terminal.

---

## 💻 Show Me Code!

### 1. Basics & Object-Oriented Programming
```mountx
mountx
classx Animal {
    let name = ""
    func init(name) {
        this.name = name
    }
    func sound() {
        return "Generic sound"
    }
}

classx Dog extends Animal {
    let breed = ""
    func init(name, breed) {
        this.name = name
        this.breed = breed
    }
    func sound() {
        return "Woof woof!"
    }
}

let d = newx Dog("Rex", "German Shepherd")
printx(d.name + " (" + d.breed + ") says: " + d.sound())
unmountx
```

### 2. High-Performance Full-Stack Web Server
```mountx
mountx
importx http_server

let app = newx http_server.App(8080)

# Logger Middleware
app.use(func(req, res) {
    printx("[HTTP] " + req.method + " " + req.path)
    return true
})

# REST API Endpoint
app.get("/api/status", func(req, res) {
    return res.json([
        ["status", "online"],
        ["engine", "MountX Native AOT"],
        ["version", "1.1.0"]
    ])
})

# HTML Welcome Page
app.get("/", func(req, res) {
    return res.html("<h1>Welcome to MountX Web Engine!</h1>")
})

printx("🚀 Server running on http://127.0.0.1:8080")
app.listen()
unmountx
```

### 3. Database Operations with SQLite ORM
```mountx
mountx
importx orm

# Define an ORM model
classx Task extends orm.Model {
    func init() {
        super("tasks", [
            ["id", "INTEGER PRIMARY KEY AUTOINCREMENT"],
            ["title", "TEXT NOT NULL"],
            ["completed", "INTEGER DEFAULT 0"]
        ])
    }
}

let tasks = newx Task()
tasks.migrate()

# Insert a new record
tasks.create([
    ["title", "Build fullstack MountX application"],
    ["completed", 1]
])

# Query records
let all_tasks = tasks.all()
printx("Total tasks in database:")
printx(all_tasks)
unmountx
```

### 4. Error Handling & Exceptions
```mountx
mountx
func safe_divide(a, b) {
    if b == 0 {
        throwx "MathError: Cannot divide by zero"
    }
    return a / b
}

tryx {
    printx("100 / 4 = " + safe_divide(100, 4))
    safe_divide(10, 0)
} catchx (err) {
    printx("Caught expected exception: " + err)
}
unmountx
```

---

## 📦 Package Management with `mxpm`

MountX includes **`mxpm`**, a lightweight, self-contained package and project manager:

```bash
# Initialize a new MountX project
mxpm init my-app

# Add and install a library
mxpm add json-utils

# Run scripts defined in mountx.json
mxpm run start
mxpm run test

# Pack a reusable package into a .mxpkg tarball archive
mxpm pack

# Launch the interactive WebAssembly playground
mxpm playground
```

Packages are installed into `libs/` and resolved automatically by MountX's import resolver (`importx <pkg>`). Full guide: [docs/MXPM_GUIDE.md](file:///home/theyad75/Desktop/antmountxlang/docs/MXPM_GUIDE.md).

---

## 📚 Standard Library (43 AOT Modules)

MountX ships with **43 built-in modules** embedded directly inside the native runtime (`gate2/g1/stdlib_aot.c`), allowing instantaneous imports with zero filesystem overhead:

| Category | Modules | Key Functionalities |
|:---|:---|:---|
| **Core** | `type`, `error`, `iter`, `assert`, `random`, `enum`, `exception`, `tuple` | Type introspection, reflection, random seeds, iterations, custom exceptions. |
| **Data & Storage** | `dict`, `map`, `set`, `hashtable`, `db`, `orm` | Key-value dictionaries, hash sets, SQLite 3 engine, Active-Record query builder & ORM. |
| **Data Structures** | `stack`, `queue`, `linkedlist`, `bst`, `collections`, `graph` | Binary search trees, FIFO queues, LIFO stacks, directed/undirected graphs. |
| **Text & Parsing** | `string`, `json`, `array`, `arrayfull`, `regex`, `buffer`, `url` | JSON serializer/parser, POSIX regex matching, URL parser, binary memory buffers. |
| **Math** | `math`, `mathfull` | Floating-point math, trigonometry, logarithms, statistical combinatorics. |
| **System & Network**| `io`, `sys`, `net`, `os`, `time`, `datetime`, `socket`, `http_server`, `args`, `process` | Berkeley TCP sockets, HTTP framework, process spawning/piping, CLI flag parsing. |
| **Bundle** | `stdlib` | All-in-one bundle: `importx stdlib` exposes standard library utilities directly. |

---

## 🏗️ Cross-Platform Compilation Pipeline

MountX can be compiled for three targets from a single C99 codebase:

1. **Linux x86-64 (`mountx-native`)**:
   ```bash
   gcc -std=c99 -O2 -Wall -Wextra -DMX_SUBSTRATE -DMX_BOTH \
       -I gate2/g1 gate2/g1/mini.c gate2/g1/interp.emitted.c gate2/g1/stdlib_aot.c gate2/g1/sqlite3.c \
       -lm -lpthread -ldl -o mountx-native
   ```
2. **Windows x86-64 (`mountx.exe`)**:
   Cross-compiled using MinGW-w64 or Zig:
   ```bash
   ./build_win.sh
   ```
3. **WebAssembly / Browser (`web/mountx.wasm`)**:
   Compiled using Emscripten:
   ```bash
   ./tools/build_wasm.sh
   ```

Detailed architectural explanation: [docs/HOW_MOUNTX_NATIVE_WORKS.md](file:///home/theyad75/Desktop/antmountxlang/docs/HOW_MOUNTX_NATIVE_WORKS.md).

---

## 🧪 Testing & Verification Protocol

Reliability is backed by an automated differential regression test suite:

* **Regression Suite (`host/fbtest.mx`)**:
  **343 assertions** covering every module, inheritance hierarchies, SQLite transactions, live TCP networking, regex matching, and native errors.
  ```bash
  ./mountx-native host/fbtest.mx
  # Output: OK - 343 check(s) passed (100% OK)
  ```
* **Documentation Verification (`docs_verify.py`)**:
  Runs every executable code fence in `docs/` and validates output byte-for-byte:
  ```bash
  python3 docs_verify.py
  # Output: 60 example(s), 0 failing
  ```
* **WebAssembly Browser Tests (`web/test_wasm.js`)**:
  Validates `mountx.wasm` across arithmetic, loops, recursion, and classes:
  ```bash
  node web/test_wasm.js
  # Output: ALL MOUNTX WEBASSEMBLY TESTS PASSED (100% OK)
  ```

---

## 📁 Repository Layout

```
mountx                  # Executable CLI wrapper & REPL
mountx-native           # High-performance compiled C native binary (Linux)
mxpm                    # Official package manager CLI
workbench               # GTK 4 + Libadwaita dark GUI studio
host/                   # Reference implementation (main.py, lexer.py, parser.py, interpreter.py)
gate2/g1/               # C Substrate runtime (mini.c, interp.emitted.c, stdlib_aot.c, sqlite3.c)
stdlib/                 # 43 Standard library modules (core/, data/, structs/, text/, math/, system/)
web/                    # WebAssembly distribution (mountx.wasm, mountx.js, index.html)
docs/                   # Comprehensive language guides, architecture, and tutorials
examples/               # 66+ Runnable demonstration programs across all categories
tools/                  # Build scripts (build_wasm.sh, serve_web.sh, mxpm, vscode extension)
```

---

## 📄 License

MountX is licensed under the [MIT License](LICENSE).  
Copyright (C) 2026-2027 Ziad Mohsin. All Rights Reserved.
