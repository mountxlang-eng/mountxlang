#!/usr/bin/env python3
# Copyright (C) 2026-2027 Ziad Mohsin. All Rights Reserved.
"""
tools/mxpm/mxpm.py - Official MountX Package Manager (mxpm)

Manages MountX libraries, projects, dependencies, packaging, and scripts.
Packages are installed into 'libs/', which is automatically resolved by MountX
import search order (importx <pkg>).
"""

import os
import sys
import json
import shutil
import tarfile
import argparse
import subprocess
from pathlib import Path

VERSION = "1.0.0"
CONFIG_FILE = "mountx.json"
LIBS_DIR = "libs"

# Terminal ANSI colors
GREEN = "\033[1;32m"
BLUE = "\033[1;34m"
CYAN = "\033[1;36m"
YELLOW = "\033[1;33m"
RED = "\033[1;31m"
BOLD = "\033[1m"
RESET = "\033[0m"

def log_info(msg):
    print(f"{BLUE}[mxpm]{RESET} {msg}")

def log_success(msg):
    print(f"{GREEN}[mxpm]{RESET} {msg}")

def log_warn(msg):
    print(f"{YELLOW}[mxpm WARNING]{RESET} {msg}")

def log_error(msg):
    print(f"{RED}[mxpm ERROR]{RESET} {msg}")

def load_config(cwd=None):
    target = os.path.join(cwd or os.getcwd(), CONFIG_FILE)
    if not os.path.exists(target):
        return None
    try:
        with open(target, "r", encoding="utf-8") as fp:
            return json.load(fp)
    except Exception as e:
        log_error(f"Failed to read {CONFIG_FILE}: {e}")
        return None

def save_config(cfg, cwd=None):
    target = os.path.join(cwd or os.getcwd(), CONFIG_FILE)
    try:
        with open(target, "w", encoding="utf-8") as fp:
            json.dump(cfg, fp, indent=2, ensure_ascii=False)
            fp.write("\n")
        return True
    except Exception as e:
        log_error(f"Failed to write {CONFIG_FILE}: {e}")
        return False

# ---------------------------------------------------------------------------
#  Scaffolding & Templates Engine
# ---------------------------------------------------------------------------
def scaffold_template(target_dir, name, template="default", force=False):
    os.makedirs(target_dir, exist_ok=True)
    cfg_path = os.path.join(target_dir, CONFIG_FILE)
    if os.path.exists(cfg_path) and not force:
        log_warn(f"{CONFIG_FILE} already exists in {target_dir}. Use --force to overwrite.")
        return 0

    pkg_ident = name.replace("-", "_")
    os.makedirs(os.path.join(target_dir, LIBS_DIR), exist_ok=True)

    if template in ("web", "api"):
        cfg = {
            "name": name,
            "version": "1.0.0",
            "description": f"Full-stack web application powered by MountX",
            "main": "main.mx",
            "author": os.environ.get("USER", "Ziad Mohsin"),
            "license": "MIT",
            "scripts": {
                "start": "mountx main.mx",
                "start:native": "./mountx-native main.mx",
                "test": "mountx test.mx"
            },
            "dependencies": {},
            "devDependencies": {}
        }
        main_content = f"""mountx
importx http_server
importx orm

# -------------------------------------------------------------
# 🚀 {name} - Full-Stack Web Application
# Powered by http_server.mx & orm.mx (SQLite)
# -------------------------------------------------------------

# 1. Connect to SQLite database
orm.connect("app.db")
orm.default_db().exec("CREATE TABLE IF NOT EXISTS items (id INTEGER PRIMARY KEY AUTOINCREMENT, name TEXT NOT NULL);")

# 2. Initialize Web App
let app = http_server.new_app()

# 3. Logger Middleware
func request_logger(req, res) {{
    printx("📌 [HTTP " + req.method + "] " + req.path)
}}
app.use(request_logger)

# 4. Route: Home Page (HTML)
func handle_home(req, res) {{
    let html = "<!DOCTYPE html><html><head><meta charset='utf-8'><title>{name}</title>" +
        "<style>body{{background:#0b0f19;color:#f1f5f9;font-family:sans-serif;padding:40px;text-align:center}}" +
        "h1{{color:#38bdf8}}a{{color:#818cf8;text-decoration:none}}.box{{background:#1e293b;padding:20px;border-radius:12px;display:inline-block;margin:20px;text-align:left}}</style>" +
        "</head><body><h1>🚀 Welcome to {name}</h1>" +
        "<p>Full-stack web application running on MountX native runtime.</p>" +
        "<div class='box'><h3>Available Endpoints:</h3><ul>" +
        "<li><a href='/api/items'>GET /api/items</a> - View items in SQLite</li>" +
        "<li><a href='/api/health'>GET /api/health</a> - Service health status</li>" +
        "</ul></div></body></html>"
    res.html(html)
}}
app.get("/", handle_home)

# 5. Route: Health Check
func handle_health(req, res) {{
    res.json([
        ["status", "healthy"],
        ["framework", "MountX HTTP Framework"],
        ["version", "1.1.0"]
    ])
}}
app.get("/api/health", handle_health)

# 6. Route: Items API
func handle_get_items(req, res) {{
    let all_items = orm.table("items").get_pairs()
    res.json([
        ["status", "ok"],
        ["count", orm.table("items").count()],
        ["items", all_items]
    ])
}}
app.get("/api/items", handle_get_items)

printx("==================================================")
printx("  🚀 Server started: http://127.0.0.1:8080")
printx("  📁 Database: SQLite (app.db)")
printx("==================================================")
app.listen(8080, "0.0.0.0", nullx)
unmountx
"""
        with open(os.path.join(target_dir, "main.mx"), "w", encoding="utf-8") as fp:
            fp.write(main_content)

        pub_dir = os.path.join(target_dir, "public")
        os.makedirs(pub_dir, exist_ok=True)
        with open(os.path.join(pub_dir, "index.html"), "w", encoding="utf-8") as fp:
            fp.write(f"""<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <title>{name}</title>
</head>
<body style="background:#0f172a;color:#f8fafc;font-family:sans-serif;text-align:center;padding:50px;">
    <h1>🚀 {name}</h1>
    <p>MountX Web Application Frontend</p>
</body>
</html>
""")

    elif template == "cli":
        cfg = {
            "name": name,
            "version": "1.0.0",
            "description": f"High-performance CLI tool written in MountX",
            "main": "main.mx",
            "author": os.environ.get("USER", "Ziad Mohsin"),
            "license": "MIT",
            "scripts": {
                "start": "mountx main.mx --help",
                "test": "mountx test.mx"
            },
            "dependencies": {},
            "devDependencies": {}
        }
        main_content = f"""mountx
importx args
importx sys

# -------------------------------------------------------------
# 🛠️ {name} - Command-Line Tool in MountX
# -------------------------------------------------------------

let parser = args.create_parser("{name}", "High-performance CLI tool built with MountX")
parser.add_flag("verbose", "v", "Enable verbose debug output")
parser.add_opt("name", "n", "World", "Name of recipient to greet")
parser.add_arg("action", "Action command (greet, info, help)")

parser.parse(args.get_args())

if parser.has("verbose") {{
    printx("🔍 [Debug] Verbose mode enabled")
}}

let action = parser.get("action")
if action == nullx or action == "help" {{
    printx(parser.help())
}} else if action == "greet" {{
    let target = parser.get("name")
    printx("Hello, " + target + "! Greetings from MountX CLI.")
}} else if action == "info" {{
    printx("Tool: {name} | Engine: MountX Native Runtime v1.1.0")
}} else {{
    printx("Unknown action: " + action)
    printx(parser.help())
}}
unmountx
"""
        with open(os.path.join(target_dir, "main.mx"), "w", encoding="utf-8") as fp:
            fp.write(main_content)

    elif template == "lib":
        cfg = {
            "name": name,
            "version": "1.0.0",
            "description": f"A reusable MountX library",
            "main": "index.mx",
            "author": os.environ.get("USER", "Ziad Mohsin"),
            "license": "MIT",
            "scripts": {
                "test": "mountx test.mx"
            },
            "dependencies": {},
            "devDependencies": {}
        }
        index_content = f"""mountx
# index.mx - Main export entry for library {name}

func add(a, b) {{
    return a + b
}}

func greet(who) {{
    return "Hello, " + who + "! Welcome to {name}."
}}
unmountx
"""
        with open(os.path.join(target_dir, "index.mx"), "w", encoding="utf-8") as fp:
            fp.write(index_content)

        test_content = f"""mountx
importx assert
importx index

printx("🧪 Running test suite for {name}...")

assert.assert_true(index.add(10, 20) == 30, "add(10, 20) should be 30")
assert.assert_true(index.greet("Ziad") == "Hello, Ziad! Welcome to {name}.", "greet assertion")

printx("✅ All unit tests passed!")
unmountx
"""
        with open(os.path.join(target_dir, "test.mx"), "w", encoding="utf-8") as fp:
            fp.write(test_content)

    else:  # default / minimal
        cfg = {
            "name": name,
            "version": "1.0.0",
            "description": "A MountX application or library",
            "main": "main.mx",
            "author": os.environ.get("USER", "Ziad Mohsin"),
            "license": "MIT",
            "scripts": {
                "start": "mountx main.mx",
                "test": "mountx test.mx"
            },
            "dependencies": {},
            "devDependencies": {}
        }
        main_content = f"""mountx
# main.mx - Created by mxpm
printx("Hello from MountX project: " + \"{name}\")
unmountx
"""
        with open(os.path.join(target_dir, "main.mx"), "w", encoding="utf-8") as fp:
            fp.write(main_content)

    # Save manifest
    save_config(cfg, cwd=target_dir)

    # Save default .gitignore
    git_ignore = os.path.join(target_dir, ".gitignore")
    if not os.path.exists(git_ignore):
        with open(git_ignore, "w", encoding="utf-8") as fp:
            fp.write("libs/\n*.db\ntmp/\n*.log\n")

    log_success(f"Initialized new MountX package in {BOLD}{cfg_path}{RESET}")
    print(f"  • Template: {CYAN}{template}{RESET}")
    print(f"  • Run: {BOLD}mxpm run start{RESET} (or mountx {cfg['main']})")
    return 0

def resolve_template_name(args):
    if getattr(args, "web", False):
        return "web"
    if getattr(args, "cli", False):
        return "cli"
    if getattr(args, "lib", False):
        return "lib"
    return getattr(args, "template", "default") or "default"

# -----------------------------------------------------------
#  Commands: init & new
# -----------------------------------------------------------
def cmd_init(args):
    cwd = os.getcwd()
    name = args.name or os.path.basename(cwd) or "mountx-app"
    template = resolve_template_name(args)
    return scaffold_template(cwd, name, template=template, force=args.force)

def cmd_new(args):
    target_dir = os.path.abspath(args.name)
    name = os.path.basename(target_dir) or "mountx-app"
    template = resolve_template_name(args)
    return scaffold_template(target_dir, name, template=template, force=args.force)

# ---------------------------------------------------------------------------
#  Command: add / install <pkg>
# ---------------------------------------------------------------------------
def expose_package_entry(libs_path, dest_dir, pkg_name, manifest=None):
    pkg_ident = pkg_name.replace("-", "_")
    main_file = manifest.get("main") if manifest else None
    
    # If package name has hyphens, ensure libs/<pkg_ident> exists too
    if pkg_ident != pkg_name:
        ident_dir = os.path.join(libs_path, pkg_ident)
        if not os.path.exists(ident_dir):
            try:
                os.symlink(dest_dir, ident_dir)
            except Exception:
                shutil.copytree(dest_dir, ident_dir, dirs_exist_ok=True)

    # Expose main entry point as libs/<pkg_ident>.mx
    entry_candidates = []
    if main_file and main_file not in ("main.mx", "index.mx"):
        entry_candidates.append(os.path.join(dest_dir, main_file))
    entry_candidates.extend([
        os.path.join(dest_dir, f"{pkg_ident}.mx"),
        os.path.join(dest_dir, f"{pkg_name}.mx"),
    ])
    if main_file:
        entry_candidates.append(os.path.join(dest_dir, main_file))
    entry_candidates.extend([
        os.path.join(dest_dir, "index.mx"),
        os.path.join(dest_dir, "main.mx")
    ])

    for cand in entry_candidates:
        if os.path.isfile(cand):
            link_target = os.path.join(libs_path, f"{pkg_ident}.mx")
            if not os.path.exists(link_target):
                try:
                    os.symlink(cand, link_target)
                except Exception:
                    shutil.copyfile(cand, link_target)
            break

def cmd_add(args):
    cwd = os.getcwd()
    cfg = load_config(cwd)
    if cfg is None:
        log_warn(f"No {CONFIG_FILE} found. Creating one first...")
        cmd_init(argparse.Namespace(name=None, force=False))
        cfg = load_config(cwd)

    pkg_spec = args.package
    libs_path = os.path.join(cwd, LIBS_DIR)
    os.makedirs(libs_path, exist_ok=True)

    # 1. Local .mxpkg archive
    if pkg_spec.endswith(".mxpkg") or pkg_spec.endswith(".tar.gz") or pkg_spec.endswith(".tgz"):
        if not os.path.exists(pkg_spec):
            log_error(f"Archive file not found: {pkg_spec}")
            return 1
        with tarfile.open(pkg_spec, "r:*") as tar:
            # Inspect manifest inside archive
            manifest = None
            try:
                mf = tar.extractfile("mountx.json")
                if mf:
                    manifest = json.load(mf)
            except Exception:
                pass

            pkg_name = manifest.get("name") if manifest else Path(pkg_spec).stem.split("-")[0]
            pkg_version = manifest.get("version", "1.0.0") if manifest else "1.0.0"
            dest = os.path.join(libs_path, pkg_name)
            os.makedirs(dest, exist_ok=True)
            tar.extractall(dest)
            expose_package_entry(libs_path, dest, pkg_name, manifest)
            
            dep_sec = "devDependencies" if args.dev else "dependencies"
            cfg[dep_sec][pkg_name] = f"file:{os.path.abspath(pkg_spec)}"
            save_config(cfg)
            log_success(f"Installed {BOLD}{pkg_name}@{pkg_version}{RESET} into {LIBS_DIR}/{pkg_name}/")
            return 0

    # 2. Local single .mx file
    if pkg_spec.endswith(".mx") and os.path.exists(pkg_spec):
        pkg_name = Path(pkg_spec).stem
        dest = os.path.join(libs_path, f"{pkg_name}.mx")
        shutil.copyfile(pkg_spec, dest)
        dep_sec = "devDependencies" if args.dev else "dependencies"
        cfg[dep_sec][pkg_name] = f"file:{os.path.abspath(pkg_spec)}"
        save_config(cfg)
        log_success(f"Copied {BOLD}{pkg_name}.mx{RESET} to {LIBS_DIR}/{pkg_name}.mx")
        return 0

    # 3. Local directory
    if os.path.isdir(pkg_spec):
        pkg_cfg = load_config(pkg_spec)
        pkg_name = pkg_cfg.get("name") if pkg_cfg else os.path.basename(os.path.abspath(pkg_spec))
        dest = os.path.join(libs_path, pkg_name)
        if os.path.exists(dest):
            shutil.rmtree(dest)
        shutil.copytree(pkg_spec, dest, ignore=shutil.ignore_patterns(".git", "libs", "__pycache__"))
        expose_package_entry(libs_path, dest, pkg_name, pkg_cfg)
        dep_sec = "devDependencies" if args.dev else "dependencies"
        cfg[dep_sec][pkg_name] = f"file:{os.path.abspath(pkg_spec)}"
        save_config(cfg)
        log_success(f"Linked local directory {BOLD}{pkg_name}{RESET} into {LIBS_DIR}/{pkg_name}/")
        return 0

    # 4. Standard library or known module reference
    # Check if user meant installing standard template or registry package
    log_info(f"Adding dependency declaration for {BOLD}{pkg_spec}{RESET}...")
    dep_sec = "devDependencies" if args.dev else "dependencies"
    cfg[dep_sec][pkg_spec] = "latest"
    save_config(cfg)
    log_success(f"Added {pkg_spec} to {dep_sec} in {CONFIG_FILE}")
    return 0

# ---------------------------------------------------------------------------
#  Command: install (without args)
# ---------------------------------------------------------------------------
def cmd_install_all(args):
    cwd = os.getcwd()
    cfg = load_config(cwd)
    if cfg is None:
        log_error(f"No {CONFIG_FILE} found in {cwd}.")
        return 1

    deps = cfg.get("dependencies", {})
    dev_deps = cfg.get("devDependencies", {})
    all_deps = {**deps, **dev_deps}

    if not all_deps:
        log_info("No dependencies listed in mountx.json.")
        return 0

    log_info(f"Verifying {len(all_deps)} dependencies...")
    libs_path = os.path.join(cwd, LIBS_DIR)
    os.makedirs(libs_path, exist_ok=True)

    installed_count = 0
    for pkg_name, src in all_deps.items():
        dest_dir = os.path.join(libs_path, pkg_name)
        dest_file = os.path.join(libs_path, f"{pkg_name}.mx")
        
        if os.path.exists(dest_dir) or os.path.exists(dest_file):
            print(f"  ✓ {pkg_name} ({src}) - up to date")
            installed_count += 1
            continue

        if src.startswith("file:"):
            local_path = src[5:]
            if os.path.isfile(local_path):
                if local_path.endswith(".mxpkg") or local_path.endswith(".tar.gz"):
                    with tarfile.open(local_path, "r:*") as tar:
                        os.makedirs(dest_dir, exist_ok=True)
                        tar.extractall(dest_dir)
                    manifest = load_config(dest_dir)
                    expose_package_entry(libs_path, dest_dir, pkg_name, manifest)
                else:
                    shutil.copyfile(local_path, dest_file)
                print(f"  + {pkg_name} restored from {local_path}")
                installed_count += 1
            elif os.path.isdir(local_path):
                shutil.copytree(local_path, dest_dir, ignore=shutil.ignore_patterns(".git", "libs", "__pycache__"))
                manifest = load_config(dest_dir)
                expose_package_entry(libs_path, dest_dir, pkg_name, manifest)
                print(f"  + {pkg_name} restored from {local_path}")
                installed_count += 1
            else:
                log_warn(f"Dependency source not found: {src}")
        else:
            print(f"  ✓ {pkg_name} (registered: {src})")
            installed_count += 1

    log_success(f"Dependencies checked: {installed_count} ready in {LIBS_DIR}/.")
    return 0

# ---------------------------------------------------------------------------
#  Command: remove / uninstall
# ---------------------------------------------------------------------------
def cmd_remove(args):
    cwd = os.getcwd()
    cfg = load_config(cwd)
    if cfg is None:
        log_error(f"No {CONFIG_FILE} found.")
        return 1

    pkg_name = args.package
    libs_path = os.path.join(cwd, LIBS_DIR)
    removed = False

    if pkg_name in cfg.get("dependencies", {}):
        del cfg["dependencies"][pkg_name]
        removed = True
    if pkg_name in cfg.get("devDependencies", {}):
        del cfg["devDependencies"][pkg_name]
        removed = True

    # Remove files
    target_dir = os.path.join(libs_path, pkg_name)
    target_file = os.path.join(libs_path, f"{pkg_name}.mx")

    if os.path.exists(target_dir):
        shutil.rmtree(target_dir)
        removed = True
    if os.path.exists(target_file):
        os.remove(target_file)
        removed = True

    if removed:
        save_config(cfg)
        log_success(f"Removed package {BOLD}{pkg_name}{RESET}")
        return 0
    else:
        log_warn(f"Package {pkg_name} was not found in {CONFIG_FILE} or {LIBS_DIR}/.")
        return 1

# ---------------------------------------------------------------------------
#  Command: list
# ---------------------------------------------------------------------------
def cmd_list(args):
    cwd = os.getcwd()
    cfg = load_config(cwd)
    name = cfg.get("name", os.path.basename(cwd)) if cfg else os.path.basename(cwd)
    version = cfg.get("version", "1.0.0") if cfg else "unknown"

    print(f"\n{BOLD}{name}@{version}{RESET} {CYAN}({cwd}){RESET}")

    deps = cfg.get("dependencies", {}) if cfg else {}
    dev_deps = cfg.get("devDependencies", {}) if cfg else {}

    libs_path = os.path.join(cwd, LIBS_DIR)
    local_files = []
    if os.path.exists(libs_path):
        local_files = os.listdir(libs_path)

    if not deps and not dev_deps and not local_files:
        print("  (no dependencies installed)")
        return 0

    if deps:
        print(f"\n{GREEN}Dependencies:{RESET}")
        for k, v in deps.items():
            status = "✓" if (k in local_files or f"{k}.mx" in local_files) else "!"
            print(f"  {status} {BOLD}{k}{RESET} ({v})")

    if dev_deps:
        print(f"\n{YELLOW}Dev Dependencies:{RESET}")
        for k, v in dev_deps.items():
            status = "✓" if (k in local_files or f"{k}.mx" in local_files) else "!"
            print(f"  {status} {BOLD}{k}{RESET} ({v})")

    installed_not_listed = [f for f in local_files if f not in deps and f.replace(".mx", "") not in deps and f not in dev_deps and f.replace(".mx", "") not in dev_deps]
    if installed_not_listed:
        print(f"\n{BLUE}Local packages in {LIBS_DIR}/:{RESET}")
        for f in installed_not_listed:
            print(f"  • {f}")

    print()
    return 0

# ---------------------------------------------------------------------------
#  Command: pack
# ---------------------------------------------------------------------------
def cmd_pack(args):
    cwd = os.getcwd()
    cfg = load_config(cwd)
    if cfg is None:
        log_error(f"Cannot pack without a valid {CONFIG_FILE}.")
        return 1

    name = cfg.get("name", "package").replace("/", "_").replace("@", "")
    version = cfg.get("version", "1.0.0")
    tar_filename = f"{name}-{version}.mxpkg"
    tar_path = os.path.join(cwd, tar_filename)

    ignored_dirs = {".git", "libs", "tmp", "__pycache__", "node_modules"}
    ignored_exts = {".tar.gz", ".mxpkg", ".o", ".pyc", ".db"}

    log_info(f"Packaging {BOLD}{name}@{version}{RESET} into {tar_filename}...")
    with tarfile.open(tar_path, "w:gz") as tar:
        for root, dirs, files in os.walk(cwd):
            dirs[:] = [d for d in dirs if d not in ignored_dirs]
            for file in files:
                ext = os.path.splitext(file)[1]
                if ext in ignored_exts or file == tar_filename:
                    continue
                full_path = os.path.join(root, file)
                rel_path = os.path.relpath(full_path, cwd)
                tar.add(full_path, arcname=rel_path)

    size_kb = os.path.getsize(tar_path) / 1024.0
    log_success(f"Created package archive: {BOLD}{tar_filename}{RESET} ({size_kb:.1f} KB)")
    return 0

# ---------------------------------------------------------------------------
#  Command: run
# ---------------------------------------------------------------------------
def cmd_run(args):
    cwd = os.getcwd()
    cfg = load_config(cwd)
    if cfg is None:
        log_error(f"No {CONFIG_FILE} found.")
        return 1

    scripts = cfg.get("scripts", {})
    script_name = args.script

    if not script_name:
        print(f"\n{BOLD}Available scripts in {CONFIG_FILE}:{RESET}")
        for s, cmd in scripts.items():
            print(f"  • {BOLD}{s}{RESET}: {CYAN}{cmd}{RESET}")
        print()
        return 0

    if script_name not in scripts:
        log_error(f"Script '{script_name}' not defined in {CONFIG_FILE}.")
        return 1

    cmd_str = scripts[script_name]
    log_info(f"Running '{script_name}': {BOLD}{cmd_str}{RESET}")
    
    # Resolve repo root relative to mxpm.py to make mountx and mountx-native available
    repo_dir = Path(__file__).resolve().parent.parent.parent
    env = os.environ.copy()
    env["PATH"] = f"{repo_dir}:{env.get('PATH', '')}"
    if "MOUNTX_STD" not in env:
        env["MOUNTX_STD"] = str(repo_dir / "stdlib")

    ret = subprocess.call(cmd_str, shell=True, cwd=cwd, env=env)
    return ret

# ---------------------------------------------------------------------------
#  Command: playground (web)
# ---------------------------------------------------------------------------
def cmd_playground(args):
    repo_dir = Path(__file__).resolve().parent.parent.parent
    web_dir = repo_dir / "web"
    port = getattr(args, "port", 8088) or 8088
    
    if not (web_dir / "mountx.wasm").exists():
        log_warn("mountx.wasm not found in web/. Building WebAssembly target now...")
        subprocess.run(["bash", str(repo_dir / "tools" / "build_wasm.sh")])

    log_success(f"Launching MountX WebAssembly Playground at {BOLD}http://localhost:{port}{RESET}")

    import threading
    import webbrowser
    threading.Timer(1.0, lambda: webbrowser.open(f"http://localhost:{port}")).start()

    import http.server
    import socketserver

    class CustomHandler(http.server.SimpleHTTPRequestHandler):
        def __init__(self, *a, **kw):
            super().__init__(*a, directory=str(web_dir), **kw)
        def log_message(self, format, *args):
            pass # Quiet mode

    try:
        with socketserver.TCPServer(("", port), CustomHandler) as httpd:
            print(f"🚀 Serving live playground on http://localhost:{port} (Press Ctrl+C to stop)")
            httpd.serve_forever()
    except KeyboardInterrupt:
        print("\nPlayground server stopped.")
        return 0
    except Exception as e:
        log_error(f"Failed to start playground server: {e}")
        return 1

# ---------------------------------------------------------------------------
#  Main CLI Entry Point
# ---------------------------------------------------------------------------
def main():
    parser = argparse.ArgumentParser(
        prog="mxpm",
        description="MountX Package Manager (mxpm) - Fast, lightweight package & project manager for MountX."
    )
    parser.add_argument("-v", "--version", action="version", version=f"mxpm {VERSION}")
    
    subparsers = parser.add_subparsers(dest="command", help="Available commands")

    # new
    p_new = subparsers.add_parser("new", help="Create and scaffold a new MountX project directory")
    p_new.add_argument("name", help="Project directory name")
    p_new.add_argument("-t", "--template", choices=["default", "web", "api", "cli", "lib"], default="default", help="Project template (default, web, cli, lib)")
    p_new.add_argument("--web", action="store_true", help="Shortcut for --template web")
    p_new.add_argument("--cli", action="store_true", help="Shortcut for --template cli")
    p_new.add_argument("--lib", action="store_true", help="Shortcut for --template lib")
    p_new.add_argument("-f", "--force", action="store_true", help="Overwrite existing files")

    # init
    p_init = subparsers.add_parser("init", help="Initialize a project manifest and template in the current directory")
    p_init.add_argument("name", nargs="?", default=None, help="Package name (default: current directory name)")
    p_init.add_argument("-t", "--template", choices=["default", "web", "api", "cli", "lib"], default="default", help="Project template (default, web, cli, lib)")
    p_init.add_argument("--web", action="store_true", help="Shortcut for --template web")
    p_init.add_argument("--cli", action="store_true", help="Shortcut for --template cli")
    p_init.add_argument("--lib", action="store_true", help="Shortcut for --template lib")
    p_init.add_argument("-f", "--force", action="store_true", help="Overwrite existing manifest")

    # add / install
    p_add = subparsers.add_parser("add", help="Add and install a dependency (file, dir, archive, or module)")
    p_add.add_argument("package", help="Package specification (.mx file, dir, .mxpkg archive, or package name)")
    p_add.add_argument("-D", "--dev", action="store_true", help="Save to devDependencies")

    p_install = subparsers.add_parser("install", help="Install dependencies from mountx.json or add package")
    p_install.add_argument("package", nargs="?", default=None, help="Optional package to add")
    p_install.add_argument("-D", "--dev", action="store_true", help="Save to devDependencies")

    # remove
    p_remove = subparsers.add_parser("remove", help="Remove an installed package")
    p_remove.add_argument("package", help="Name of package to remove")

    # list
    subparsers.add_parser("list", help="List installed dependencies")

    # pack
    subparsers.add_parser("pack", help="Bundle package into a redistributable .mxpkg archive")

    # run
    p_run = subparsers.add_parser("run", help="Run a custom script defined in mountx.json")
    p_run.add_argument("script", nargs="?", default=None, help="Script name (e.g., start, test)")

    # playground / web
    p_play = subparsers.add_parser("playground", aliases=["web"], help="Launch interactive WebAssembly browser playground")
    p_play.add_argument("-p", "--port", type=int, default=8088, help="Port to serve on (default: 8088)")

    args, unknown = parser.parse_known_args()

    if not args.command:
        parser.print_help()
        return 0

    if args.command == "new":
        return cmd_new(args)
    elif args.command == "init":
        return cmd_init(args)
    elif args.command == "add":
        return cmd_add(args)
    elif args.command == "install":
        if args.package:
            return cmd_add(args)
        else:
            return cmd_install_all(args)
    elif args.command == "remove":
        return cmd_remove(args)
    elif args.command == "list":
        return cmd_list(args)
    elif args.command == "pack":
        return cmd_pack(args)
    elif args.command == "run":
        return cmd_run(args)
    elif args.command in ("playground", "web"):
        return cmd_playground(args)
    else:
        parser.print_help()
        return 0

if __name__ == "__main__":
    sys.exit(main() or 0)
