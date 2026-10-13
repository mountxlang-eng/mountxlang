<!-- Copyright (C) 2026-2027 Ziad Mohsin. All Rights Reserved. -->
# MountX Language Developers Guide (DEVELOPER_README) - FINAL UPDATED

Welcome to the architectural guide for the **MountX** project. This document aims to provide a comprehensive and fast engineering reference for any software developer or AI Agent (AI Agent) to understand the system structure, execution mechanisms, build paths, and how to contribute to the development of the language and its standard libraries.

---

## 1. Project Overview (Project Overview)

### What is MountX Language?
**MountX** is a Dynamically-Typed, lightweight scripting programming language, based in its execution on **Tree-Walking Interpreter**, and is fully **Self-Hosted**:
- Its first phase began with an integrated software reference in Python language (`host/`) to be as a standard specification (Oracle/Specification) for the language.
- The Lexer, Parser, and Interpreter were rewritten in MountX language itself (`gate1/`).
- The MountX interpreter written in MountX was compiled into standard C language code (`interp.emitted.c`) relying on a low-level runtime substrate (`mini.c`), which provides a fast and independent Native Executable environment without any external dependencies except the standard C library and the embedded SQLite library.
- The language integrates a rich standard library containing **43 software modules**, and relies on **AOT Pre-compiled Stdlib** technology to ensure loading the AST in memory immediately without any delay resulting from reading from disk.

### Current Performance Status and Capabilities (Performance & Capabilities)
1. **Super Execution Speed**: The native interpreter compiled to C (`mountx-native` / `mountx.exe`) outperforms the reference Python interpreter by rates ranging between **5x to 20x** in arithmetic operations, tree traversal, and function calls.
2. **Smart Memory Management**:
   - Use of Arena Allocator for the life cycles of code nodes.
   - Dedicated memory for small local environments (`small_b[8]`) to reduce `malloc/free` calls.
   - Loop Environment Reuse in loops (`while`, `loop`, `forin`) and zeroing pointers without re-allocating memory with each cycle.
3. **Strict Conformance and Testing**: The native interpreter passes 100% of the strict compatibility tests embedded in `host/fbtest.mx` (**343 consecutive checks** covering all language features and exceptional and diagnostic operations).
4. **Cross-Platform Support**: The native interpreter works with full efficiency on both Linux and Windows systems (x86-64).

---

## 2. Directory and File Structure (Directory & File Structure)

The project is divided into strictly defined structural units, each performs a core function in the language life cycle:

```
antmountxlang/
├── gate1/                  # Self-Hosting stage (Self-Hosting Frontend & Interpreter)
├── gate2/                  # Compiling stage and low substrate (Compiled Substrate & Emitter)
│   ├── g1/                 # C code for substrate and generated interpreter and AOT standard library
│   └── g2/                 # Compiler tools and code generator (compiler.mx)
├── shared/
│   └── stdlib/             # Official standard library for MountX language (.mx)
├── host/                   # Reference interpreter written in Python + fbtest.mx tests
├── tools/                  # AOT generation tools and code editors extensions (VS Code Extension)
├── examples/               # Comprehensive code examples and live applications in MountX language
├── docs/                   # Architectural documentation and language documentation and guides
├── assets/                 # Official icons and logos in all sizes
├── bench.sh                # Benchmark comparison script
├── build_win.sh            # Cross-compilation script for Windows system
└── DEVELOPER_README.md     # Architectural guide for developers (this file)
```

### Purpose of Main Directories:
- **`gate1/`**: Home of self-hosting; contains the Lexer and Parser and language interpreter written in MountX language (`lexer.mx`, `parser.mx`, `interp.mx`), and is the basis that is compiled to C.
- **`gate2/g1/`**: Heart of the native environment (Native C Substrate); contains the substrate file `mini.c`, and headers `mini.h`, and the generated interpreter `interp.emitted.c`, and the embedded SQLite engine `sqlite3.c`.
- **`gate2/g2/`**: Code generator and MountX-to-C compiler (`compiler.mx`) and second stage tests.
- **`shared/stdlib/`**: Standard libraries for the language (**43 libraries**) written in MountX language, and divided functionally into: `core/`, `data/`, `math/`, `structs/`, `system/`, `text/`.
- **`host/`**: Initial reference implementation in Python language (`main.py`, `lexer.py`, `parser.py`, `interpreter.py`) in addition to regression tests `fbtest.mx`.
- **`tools/`**: Developer tools, and mainly script `aot_stdlib_gen.py` for generating C from standard libraries, and official VS Code extension.
- **`examples/`**: Rich collection of programs written in MountX to illustrate syntax, and data structures, and handling databases, and networks, and text analysis.

### Most Pivotal Files:
- **`gate2/g1/mini.c`**: Core substrate in C99 language (about 7000 lines). Provides `MxValue` values layer (numbers, strings, lists, dictionaries, copies), and environments and scopes management system (Scopes & Environments), and function calls and exception control (`setjmp/longjmp`), and native functions (Native Builtins) such as system functions, and mathematics, and databases.
- **`gate2/g1/interp.emitted.c`**: C code resulting from compiling `gate1/interp.mx` by compiler `compiler.mx`. Executes full MountX language tree interpreter logic as explicit C instructions with precise error handling.
- **`gate2/g1/stdlib_aot.c`**: C code file automatically generated by `tools/aot_stdlib_gen.py`. Contains pre-built code (Pre-built AST) for all standard library modules embedded as C constants, and exports function `mx_aot_get_prog(name)`.
- **`gate1/parser.mx` & `gate1/lexer.mx`** (or combined as frontend): Self-hosted syntactic interface for the language, responsible for converting source code to Tokens then converting it to a syntactic parsing tree compatible with interpreter specifications.

---

## 3. Work and Operation Mechanism (How It Works & Execution Flow)

### Architecture Composed of 4 Layers (4-Layer Pipeline)
The life cycle of execution of any MountX program goes according to the following path:

```
+-----------------------------------------------------------+
| 1. User code (.mx)                                        |
|    Normal source program written with MountX rules        |
+-----------------------------+-----------------------------+
                              |
                              v
+-----------------------------------------------------------+
| 2. MountX Self Interpreter (MountX Self-Hosted Frontend)  |
|    - Splitting text into tokens (Lexer)                   |
|    - Building syntax parsing tree (Recursive Descent Parser)|
+-----------------------------+-----------------------------+
                              |
                              v
+-----------------------------------------------------------+
| 3. Generated C code and substrate (Emitted C + Native Substrate)|
|    - Execution of tree nodes via interp.emitted.c         |
|    - Memory management, native functions, and value calculation via mini.c|
+-----------------------------+-----------------------------+
                              |
                              v
+-----------------------------------------------------------+
| 4. Native execution and standard library                  |
|    - Loading standard library via AOT AST from memory     |
|    - Program output and final result                      |
+-----------------------------------------------------------+
```

### Unique String Storage Technique (String Interning)
In traditional interpreted programming languages, every variable access operation (`env_get`), or method call (`mx_dot`), or key check in dictionary (`mx_dict_get`) requires full string comparison via `strcmp(a, b)` with time complexity $O(N)$.
- In MountX, unique text strings are stored and unified via a canonical pointer table (`mx_intern`) supported by a direct cache (`g_ptr_cache` with capacity 4096 entries).
- Once the string is converted to a unique pointer, comparisons are performed by comparing pointer addresses directly via `==` operator with time complexity $O(1)$.
- This gives this engine a huge boost in speed of access to variables and keys and function calls inside complex iterative loops.

### Standard Library Compiled with AOT Technology (Ahead-of-Time Stdlib)
Instead of reading standard library files from disk when writing `importx math` or `importx json` and re-parsing them syntactically each time:
1. The script `tools/aot_stdlib_gen.py` converts all `.mx` files in `shared/stdlib/` in advance to AST structures written in C language inside `stdlib_aot.c`.
2. When requesting import of a library inside the interpreter, the function `mx_aot_get_prog(name)` searches in the embedded table:
   - In case it is found, it returns the constructed AST tree immediately from memory within microseconds and without any disk reading operations (Zero Disk I/O).
   - In case it is not an embedded module, the interpreter falls back to the traditional search path from disk (`shared/stdlib/`, `libs/`, current path).

---

## 4. Commands and Libraries (Commands & Standard Library)

### Build and Compilation Commands (Terminal Build Commands)

1. **Building Native Interpreter on Linux (`mountx-native`)**:
   ```bash
   gcc -std=c99 -O2 -Wall -Wextra -DMX_SUBSTRATE -DMX_BOTH -I gate2/g1 \
       gate2/g1/mini.c gate2/g1/interp.emitted.c gate2/g1/stdlib_aot.c tmp/sqlite3_linux.o \
       -lm -lpthread -ldl -o mountx-native
   ```

2. **Building Native Interpreter on Windows (`mountx.exe`)**:
   ```bash
   ./build_win.sh
   # Or via Windows command prompt:
   build_win.bat
   ```

3. **Regenerating AOT Code for Standard Library**:
   ```bash
   python3 tools/aot_stdlib_gen.py
   ```

4. **Re-extracting Header File `mini.h` from `mini.c`**:
   ```bash
   python3 gate2/g1/mkhdr.py
   ```

### Test and Comparison Running Commands (Testing & Benchmarks)

1. **Running Comprehensive Regression Tests (Gate Test) on Native Interpreter**:
   ```bash
   ./mountx-native host/fbtest.mx
   # Expected result: OK - 343 check(s) passed
   ```

2. **Running Tests on Reference Python Interpreter**:
   ```bash
   python3 host/main.py host/fbtest.mx
   ```

3. **Running Performance Measurement and Time Comparison Tool (Benchmark Runner)**:
   ```bash
   ./bench.sh
   # Or for 3 times repetition and calculating average:
   ./bench.sh --repeat 3
   ```

4. **Verifying Validity of Official Documentation Examples**:
   ```bash
   python3 docs_verify.py
   ```

### The 43 Available Standard Libraries (`shared/stdlib/`) - FINAL

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

## 5. Guidelines for Developers (Developer Guidelines)

### How to Add a New Native C Function (Native Function) in `mini.c`
Native functions are used to link operating system capabilities and low-level C libraries to MountX language:

1. **Definition in `mini.c`**:
   - Add the native execution function in C99 style using `MxValue` signature.
   - Example:
     ```c
     static MxValue mx_native_my_feature(MxValue arg1, MxValue arg2) {
         if (arg1.tag != MX_INT || arg2.tag != MX_INT) {
             mx_raise("my_feature expects two integer arguments");
         }
         return mx_int(arg1.as.i + arg2.as.i);
     }
     ```
2. **Registration in Distribution Mechanism (Dispatcher)**:
   - Search for the native functions call point inside `mini.c` (often inside Builtins names check in call function or symbol table).
   - Ensure using appropriate naming prefix starting with underscore or distinctive name (such as `sys_`, `math_`, `str_`) to avoid any conflict with user names or standard library.
3. **Updating Header `mini.h`**:
   - Do not modify `mini.h` manually! Run extraction script:
     ```bash
     python3 gate2/g1/mkhdr.py
     ```
4. **Documentation and Testing of Addition**:
   - Add appropriate assertion checks in `host/fbtest.mx`.
   - Ensure rebuilding via `gcc` and passing all tests successfully.

### How to Add a New Standard Library with `.mx` Extension
1. **Creating Source File**:
   - Place the file inside appropriate classification in `shared/stdlib/<category>/<name>.mx`.
   - Ensure writing functions in standard MountX style and using keywords in supported format (`funcx`, `let`, `return`, `classx`).
   - Avoid naming an internal function with a name matching library name or another library name to avoid Global Namespace Collision problems.
2. **Registration in General Aggregator**:
   - If library is dedicated for automatic inclusion, add its import in `shared/stdlib/stdlib.mx`.
3. **Updating Pre-compilation Code (AOT)**:
   - Run code generator:
     ```bash
     python3 tools/aot_stdlib_gen.py
     ```
   - This command will automatically update files `gate2/g1/stdlib_aot.c` and `gate2/g1/stdlib_aot.h` and include AST tree of new library directly.
4. **Verification and Building**:
   - Recompile native version:
     ```bash
     gcc -std=c99 -O2 -Wall -Wextra -DMX_SUBSTRATE -DMX_BOTH -I gate2/g1 \
         gate2/g1/mini.c gate2/g1/interp.emitted.c gate2/g1/stdlib_aot.c tmp/sqlite3_linux.o \
         -lm -lpthread -ldl -o mountx-native
     ```
   - Add test cases for library in `host/fbtest.mx` and run `./mountx-native host/fbtest.mx` to ensure all checks give `OK`.

---

## 7. Live Networking and Sockets Structure (Native TCP Sockets & Networking)

MountX language provides native support for direct network connections via TCP protocol, which allows building live web servers and communication protocols without any external libraries:

### Native Functions in Substrate (Native Primitives):
1. `tcp_listen(port, [host])`: Opening TCP port and listening for connections with enabling `SO_REUSEADDR`.
2. `tcp_accept(server_fd)`: Waiting for incoming connection and returning array `[client_fd, client_ip]`.
3. `tcp_recv(client_fd, [max_len])`: Reading request bytes and converting it to UTF-8 text string.
4. `tcp_send(client_fd, data)`: Writing data and sending it via socket to client.
5. `tcp_close(fd)`: Closing socket and freeing resources safely.
6. `tcp_connect(host, port)`: Opening external client connection with TCP socket on specified port.

### Using Standard Library `socket` and `net`:
- **Library `socket`**:
  ```mountx
  importx socket
  let server = newx socket.TcpServer(8080, "0.0.0.0") # Starts listening immediately
  let conn = server.accept()                         # Receives live connection [cfd, ip]
  let req = socket.recv(conn, 4096)                  # Automatic unwrapping for socket
  socket.send(conn, "HTTP/1.1 200 OK\r\n\r\nHello!")
  socket.close(conn)
  server.close()
  ```
- **Library `net`**:
  Includes direct function `net.serve_http(port, handler, [max_requests])` to run web servers and distribute requests with one programming line.
- **Illustrative Example**: `examples/07_networking_and_web/61_tcp_socket_server.mx`.

---

## 8. Autonomous Programming Agent (MountX Autonomous AI Agent - `mountxai/`)

The folder `mountxai/` represents the autonomous artificial intelligence system directed to MountX language:
- **`mountxai/agent.py`**: Autonomous programming agent working with **DeepSeek** model, and equipped with three-stage self-correction engine (Self-Healing Loop):
  1. **Automatic Indexing Engine (Stdlib Catalog)**: Analyzes the **43 standard library modules** dynamically and injects precise documentation based on user request words.
  2. **Smart File Reader**: Detects paths mentioned in user text automatically and reads them as reference data.
  3. **Self-Recovery Loop**: Intercepts interpreter errors (`mountx-native`) and Traceback lines and `^` indicators and redirects them to the model to correct code and re-run it immediately until full success.

---

*This guide was prepared and updated to be the primary reference for engineers and core developers in MountX project. Final Version: 43 Modules / 343 Checks / v1.1.0*
