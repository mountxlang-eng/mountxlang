<!-- Copyright (C) 2026-2027 Ziad Mohsin. All Rights Reserved. -->
# 📦 Official Package Manager Guide (MountX Package Manager — mxpm)

**`mxpm`** is the official package and project manager for the **MountX** language. It is designed to be lightweight, fast, and 100% compatible with the standard module import structure (`importx`) in both the native compiled compiler (`mountx-native`) and the reference Python interpreter (`mountx`).

---

## 🚀 Installation and Quick Start

The tool is available directly in the repository root as an executable:

```bash
./mxpm --version
# mxpm 1.0.0
```

You can link it globally to your system or add it to your `PATH`:

```bash
export PATH="$PWD:$PATH"
mxpm --help
```

---

## 📋 Core Commands (CLI Commands)

### 1. Creating and Scaffolding Projects (`mxpm new` / `mxpm init`)

`mxpm` supports a smart templating system (Scaffolding Engine) to generate complete and ready-to-use work environments:

#### A. Create a new project in a separate folder (`mxpm new`):
```bash
# Full-stack web project with HTTP server and SQLite database
mxpm new my-web-app --web

# CLI tool with flag and option parser
mxpm new my-cli-tool --cli

# Importable and reusable library with standard tests
mxpm new my-lib --lib

# Standard minimal project
mxpm new my-app
```

#### B. Initialize a project in the current folder (`mxpm init`):
```bash
mxpm init --web      # Initialize web template
mxpm init --cli      # Initialize CLI tool template
mxpm init --lib      # Initialize library template
```

#### 📦 Available Templates and Details:
1.  **Web Template (`--web` or `-t web`)**:
    - Creates a full web app connected to `http_server.mx` server.
    - Built-in SQLite database via query engine & ORM (`orm.mx`).
    - Static UI `public/index.html`.
    - Interactive API routes (`/`, `/api/health`, `/api/items`).
    - Instant run: `mxpm run start` (or `./mountx-native main.mx`).

2.  **Command-Line Tool Template (`--cli` or `-t cli`)**:
    - Built on top of the `args.mx` argument parser.
    - Supports advanced options: flags (`--verbose`, `-v`), variables (`--name`, `-n`), and subcommands (`greet`, `info`).
    - Automatic help screen generation (`--help`).

3.  **Library Template (`--lib` or `-t lib`)**:
    - Creates the main export entry point `index.mx`.
    - Standard test suite `test.mx` based on `assert.mx`.
    - Ready for packaging and publishing via `mxpm pack`.

4.  **Default Template (`default`)**:
    - A minimal and fast starting point with a `main.mx` file.

---

### `mountx.json` Manifest File:
```json
{
  "name": "my-app",
  "version": "1.0.0",
  "description": "Full-stack web application powered by MountX",
  "main": "main.mx",
  "author": "Ziad Mohsin",
  "license": "MIT",
  "scripts": {
    "start": "mountx main.mx",
    "start:native": "./mountx-native main.mx",
    "test": "mountx test.mx"
  },
  "dependencies": {},
  "devDependencies": {}
}
```

---

### 2. Adding and Installing a Package (`mxpm add`)
`mxpm add` supports multiple package source formats:
- **Compressed Archive (`.mxpkg`)**:
  ```bash
  mxpm add /path/to/math-helpers-1.0.0.mxpkg
  ```
- **Standalone MountX File (`.mx`)**:
  ```bash
  mxpm add /path/to/helper.mx
  ```
- **Local Project Folder**:
  ```bash
  mxpm add ../another-lib
  ```
- **As a Dev Dependency** by adding the `-D` flag:
  ```bash
  mxpm add -D testing-tools.mxpkg
  ```

The tool automatically links the entry point in `libs/` so it can be imported instantly from any code:
```mx
mountx
importx math_helpers
printx(add_ten(32))
unmountx
```

---

### 3. Restoring and Installing Dependencies (`mxpm install`)
When downloading a new project that contains `mountx.json`, just run:
```bash
mxpm install
```
It checks all dependencies registered in `dependencies` and `devDependencies` and installs them immediately inside the `libs/` folder.

---

### 4. Packing the Project for Distribution (`mxpm pack`)
To bundle any library into a distributable and shareable package:
```bash
mxpm pack
```
Creates an archive in `.mxpkg` format (e.g., `math-helpers-1.0.0.mxpkg`) including source codes and headers, ignoring temporary files and `.git`.

---

### 5. Listing Installed Packages (`mxpm list`)
Displays the registered dependency tree and packages in `libs/`:
```bash
mxpm list
```
**Example Output:**
```text
my-app@1.0.0 (/home/user/my-app)

Dependencies:
  ✓ math-helpers (file:/libs/math-helpers-1.0.0.mxpkg)

Local packages in libs/:
  • math_helpers.mx
  • math-helpers
```

---

### 6. Removing a Package (`mxpm remove`)
To remove a package from the project and from `mountx.json`:
```bash
mxpm remove math-helpers
```

---

### 7. Running Custom Scripts (`mxpm run`)
To run any command registered in the `scripts` section of `mountx.json`:
```bash
mxpm run start
mxpm run test
```
And if run without arguments `mxpm run`, it displays a list of available scripts in the project.

---

## 🔒 Compatibility and Search Rules (Import Search Order)
`mxpm` relies on MountX's file search rule:
`libs/<name>.mx` -> `libs/<name>/` -> `stdlib/`.
And when installing any package that contains hyphens like `math-helpers`, a shortcut with a valid MountX identifier (`math_helpers`) is automatically provided to prevent any import errors.
