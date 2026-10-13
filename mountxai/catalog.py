# Copyright (C) 2026-2027 Ziad Mohsin. All Rights Reserved.
"""
mountxai/catalog.py — Standard library catalog scanner, dynamic documentation, and file inspection.
"""

import os
import glob
import re
from mountxai.config import REPO_ROOT, STDLIB_DIR

class StdlibCatalog:
    """فهرس ديناميكي للمكتبات القياسية يستخرج التوثيق والدوال تلقائياً من الكود المصدري."""
    def __init__(self, stdlib_dir=STDLIB_DIR):
        self.stdlib_dir = stdlib_dir
        self.modules = {}
        self.scan()

    def scan(self):
        pattern = os.path.join(self.stdlib_dir, "**", "*.mx")
        for path in glob.glob(pattern, recursive=True):
            mod_name = os.path.basename(path)[:-3]
            try:
                with open(path, "r", encoding="utf-8") as f:
                    content = f.read()

                comments = []
                for line in content.split("\n"):
                    if line.startswith("#"):
                        c = line.lstrip("#").strip()
                        if c and not c.startswith("Copyright"):
                            comments.append(c)
                    elif line.strip():
                        break
                desc = " - ".join(comments[:2]) if comments else "Standard library module"

                funcs = re.findall(r"func\s+([a-zA-Z0-9_]+)\s*\(([^)]*)\)", content)
                public_funcs = [f"{mod_name}.{fn}({args})" for fn, args in funcs if not fn.startswith("_")]
                classes = re.findall(r"classx?\s+([a-zA-Z0-9_]+)", content)

                self.modules[mod_name] = {
                    "name": mod_name,
                    "path": path,
                    "desc": desc,
                    "funcs": public_funcs,
                    "classes": classes,
                    "content": content
                }
            except Exception:
                pass

    def get_module(self, name):
        return self.modules.get(name)

    def list_modules(self):
        return sorted(list(self.modules.keys()))

    def get_relevant_docs(self, prompt):
        """مطابقة ذكية للطلب وحقن توثيق الدوال المناسبة في السياق."""
        prompt_lower = prompt.lower()
        matched = []

        keywords_map = {
            "http_server": ["http_server", "server", "router", "route", "middleware", "خادم", "سيرفر", "api", "rest", "ويب", "web app", "web framework"],
            "orm": ["orm", "model", "query", "querybuilder", "record", "sqlite", "موديل", "استعلام", "active record", "قاعدة", "بيانات"],
            "db": ["sqlite", "database", "db", "قاعدة", "بيانات", "sql", "جدول", "استعلام"],
            "net": ["http", "fetch", "web", "api", "url", "شبكة", "إنترنت", "ويب", "جلب"],
            "socket": ["socket", "tcp", "listen", "accept", "connect", "port", "مقبس", "منفذ"],
            "time": ["time", "sleep", "ms", "وقت", "ثانية", "انتظار", "تأخير"],
            "datetime": ["date", "datetime", "تاريخ", "ساعة", "تقويم"],
            "json": ["json", "parse", "stringify", "تسلسل"],
            "map": ["map", "pairs", "dictionary", "قاموس", "خريطة"],
            "math": ["math", "abs", "pow", "max", "min", "factorial", "رياضيات", "حساب"],
            "mathfull": ["sqrt", "floor", "ceil", "round", "جذر", "تقريب"],
            "string": ["string", "split", "join", "upper", "lower", "نص", "سلسلة", "حروف"],
            "regex": ["regex", "pattern", "match", "تعبير نمطي", "نمط"],
            "io": ["file", "read", "write", "ملف", "قراءة", "كتابة"],
            "os": ["env", "cwd", "dir", "بيئة", "مسار"],
            "process": ["process", "run", "cmd", "عملية", "أمر"],
            "stack": ["stack", "مكدس"],
            "queue": ["queue", "طابور"],
            "bst": ["bst", "tree", "شجرة"],
            "graph": ["graph", "رسم بياني", "عقدة"],
            "random": ["random", "عشوائي", "قرعة"]
        }

        for mod, kw_list in keywords_map.items():
            if any(kw in prompt_lower for kw in kw_list):
                if mod in self.modules:
                    matched.append(mod)

        tool_docs = []
        if any(kw in prompt_lower for kw in ["mxpm", "package", "حزمة", "مكتبة", "اعتماديات", "dependencies", "pack", "install"]):
            tool_docs.append(
                "Tool `mxpm` (MountX Package Manager):\n"
                "  Commands: mxpm init [name], mxpm add <pkg>, mxpm install, mxpm pack, mxpm list, mxpm remove, mxpm run <script>\n"
                "  Manifest: mountx.json holds name, version, scripts, dependencies.\n"
                "  Resolution: Packages install into libs/ and resolve via `importx <pkg>`."
            )
        if any(kw in prompt_lower for kw in ["wasm", "webassembly", "browser", "متصفح", "ويب اسملي"]):
            tool_docs.append(
                "Target `mountx.wasm` (WebAssembly Runtime):\n"
                "  Builder: tools/build_wasm.sh compiles to web/mountx.wasm and web/mountx.js.\n"
                "  Playground: web/index.html provides dark-mode in-browser execution with zero server.\n"
                "  JS API: window.Module.ccall('mountx_run', 'number', ['string'], [code])."
            )

        if not matched and not tool_docs:
            return ""

        docs = ["\n### DYNAMIC RELEVANT MOUNTX SPECS (From Live Source):"]
        if tool_docs:
            docs.extend(tool_docs)
        for mod in set(matched):
            info = self.modules[mod]
            docs.append(f"Module `importx {mod}` ({info['desc']}):")
            if info['classes']:
                docs.append(f"  Classes: {', '.join(info['classes'])}")
            if info['funcs']:
                docs.append(f"  Functions: {', '.join(info['funcs'][:8])}")
        return "\n".join(docs) + "\n"


def inspect_and_attach_files(prompt):
    """اكتشاف وقراءة أي ملفات محلية يشير إليها المستخدم في طلبه ودمج محتواها تلقائياً."""
    tokens = prompt.replace("'", " ").replace('"', " ").replace("`", " ").split()
    attachments = []
    seen = set()

    for tok in tokens:
        clean = tok.strip(" ,;:?()[]{}<>")
        if not clean or clean in seen:
            continue

        target_path = None
        if os.path.isabs(clean) and os.path.isfile(clean):
            target_path = clean
        elif os.path.isfile(os.path.join(REPO_ROOT, clean)):
            target_path = os.path.join(REPO_ROOT, clean)
        elif "/" in clean and os.path.isfile(os.path.join(REPO_ROOT, os.path.basename(clean))):
            target_path = os.path.join(REPO_ROOT, os.path.basename(clean))

        if target_path and target_path not in seen:
            seen.add(target_path)
            try:
                if os.path.getsize(target_path) < 500000:
                    with open(target_path, "r", encoding="utf-8", errors="replace") as f:
                        content = f.read()
                    attachments.append(
                        f"\n[LOCAL WORKSPACE FILE ATTACHMENT: {os.path.basename(target_path)}]\n"
                        f"Note: This file contains user instructions, input data, or task specifications.\n"
                        f"File content:\n```\n{content}\n```\n"
                        f"CRITICAL: Do NOT copy plain English text from the file directly into MountX! "
                        f"You must write actual executable MountX code (e.g. using `printx`, functions, etc.) "
                        f"that fulfills what this file asks for.\n"
                    )
            except Exception:
                pass

    return "\n".join(attachments)

STDLIB_CATALOG = StdlibCatalog()
