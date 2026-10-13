#!/usr/bin/env python3
# Copyright (C) 2026-2027 Ziad Mohsin. All Rights Reserved.
"""
tools/aot_stdlib_gen.py - Ahead-of-Time (AOT) Stdlib Code Generator.

Generates:
  - gate2/g1/stdlib_aot.h
  - gate2/g1/stdlib_aot.c

Pre-compiles all MountX standard library modules into C:
  1. Embedded C source code for direct memory loading in the C substrate (zero disk I/O).
  2. Pre-compiled AST constructors (mx_aot_get_prog) that construct the
     Module's Program AST in native C in microseconds, bypassing runtime
     lexing and parsing inside the MountX tree-walker.
"""

import glob
import os
import sys

REPO_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO_DIR, "host"))
from main import compile_code

def c_esc(s):
    out = []
    b_seq = s.encode("utf-8")
    for b in b_seq:
        if b == 92:  # \
            out.append("\\\\")
        elif b == 34:  # "
            out.append("\\\"")
        elif b == 10:  # \n
            out.append("\\n")
        elif b == 13:  # \r
            out.append("\\r")
        elif b == 9:  # \t
            out.append("\\t")
        elif 32 <= b <= 126:
            out.append(chr(b))
        else:
            out.append(f"\\{b:03o}")
    return "".join(out)

def emit_node_expr(d, lines, var_id):
    if d is None:
        return "aot_box_null()"
    if isinstance(d, bool):
        return f"aot_box_bool({1 if d else 0})"
    if isinstance(d, int):
        return f"aot_box_int({d}LL)"
    if isinstance(d, float):
        return f"aot_box_num({repr(d)})"
    if isinstance(d, str):
        return f'aot_box_str("{c_esc(d)}")'
    if isinstance(d, list):
        vid = var_id[0]
        var_id[0] += 1
        lines.append(f"    MxArray *a{vid} = mx_arr_new({len(d)}LL);")
        for i, item in enumerate(d):
            item_expr = emit_node_expr(item, lines, var_id)
            lines.append(f"    a{vid}->v[{i}] = {item_expr};")
        return f"aot_box_arr(a{vid})"
    if isinstance(d, dict):
        vid = var_id[0]
        var_id[0] += 1
        kind = d.get("type", d.get("kind", "FIELD" if ("name" in d and "value" in d) else ""))
        line = d.get("line", 0)
        lines.append(f'    MxValue n{vid} = aot_mk_node("{c_esc(kind)}", {line});')
        for k, v in d.items():
            if k in ("type", "line"):
                continue
            key_name = "else_b" if k == "else" else ("class_name" if k == "class" else k)
            if k == "else" and v is None:
                v = []
            v_expr = emit_node_expr(v, lines, var_id)
            lines.append(f'    aot_set_field(n{vid}, "{c_esc(key_name)}", {v_expr});')
        return f"n{vid}"
    raise ValueError(f"Unknown value in AST: {d}")

def main():
    stdlib_pattern = os.path.join(REPO_DIR, "shared", "stdlib", "**", "*.mx")
    stdlib_files = glob.glob(stdlib_pattern, recursive=True)
    stdlib_files += glob.glob(os.path.join(REPO_DIR, "shared", "stdlib", "*.mx"))
    for extra in ["lexer.mx", "parser.mx"]:
        p = os.path.join(REPO_DIR, "gate1", extra)
        if os.path.exists(p):
            stdlib_files.append(p)

    # Deduplicate modules by module name (stem)
    modules_map = {}
    for f in sorted(stdlib_files):
        stem = os.path.basename(f).replace(".mx", "")
        if stem not in modules_map:
            modules_map[stem] = f

    print(f"Found {len(modules_map)} standard library modules to pre-compile.")

    hdr_path = os.path.join(REPO_DIR, "gate2", "g1", "stdlib_aot.h")
    c_path = os.path.join(REPO_DIR, "gate2", "g1", "stdlib_aot.c")

    hdr_content = """/* Copyright (C) 2026-2027 Ziad Mohsin. All Rights Reserved.
 * stdlib_aot.h - Ahead-of-Time Pre-compiled Standard Library public interface.
 * Generated automatically by tools/aot_stdlib_gen.py - do not edit.
 */
#ifndef STDLIB_AOT_H
#define STDLIB_AOT_H

#include "mini.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Returns pre-compiled Program AST node for builtin module, or mx_null() if not found */
MxValue mx_aot_get_prog(const char *name);

/* Returns embedded source text for builtin module, or NULL if not found */
const char *mx_aot_get_source(const char *name);

#ifdef __cplusplus
}
#endif

#endif /* STDLIB_AOT_H */
"""
    with open(hdr_path, "w") as f:
        f.write(hdr_content)
    print(f"Wrote {hdr_path}")

    c_out = []
    c_out.append("/* Copyright (C) 2026-2027 Ziad Mohsin. All Rights Reserved.")
    c_out.append(" * stdlib_aot.c - Ahead-of-Time Pre-compiled Standard Library implementation.")
    c_out.append(" * Generated automatically by tools/aot_stdlib_gen.py - do not edit.")
    c_out.append(" */")
    c_out.append('#include "stdlib_aot.h"')
    c_out.append("")
    c_out.append("extern void *arena_alloc(size_t n);")
    c_out.append("")
    c_out.append("/* MxArray internal struct definition */")
    c_out.append("struct MxArray { int64_t stamp; size_t n; size_t cap; MxValue *v; };")
    c_out.append("")
    c_out.append("static inline MxValue aot_mk_node(const char *kind, int line) {")
    c_out.append("    MxGenInst *inst = (MxGenInst *)arena_alloc(sizeof(MxGenInst));")
    c_out.append("    inst->stamp = mx_next_stamp();")
    c_out.append("    inst->klass = NULL;")
    c_out.append("    inst->fields = mx_dict_new(8);")
    c_out.append("    MxValue v;")
    c_out.append("    v.tag = MX_GENINST;")
    c_out.append("    v.as.box = inst;")
    c_out.append('    mx_set_field(v, "kind", mx_str(kind));')
    c_out.append('    mx_set_field(v, "line", mx_int((int64_t)line));')
    c_out.append("    return v;")
    c_out.append("}")
    c_out.append("")
    c_out.append("static inline void aot_set_field(MxValue inst, const char *field, MxValue val) {")
    c_out.append("    mx_set_field(inst, field, val);")
    c_out.append("}")
    c_out.append("")
    c_out.append("static inline MxValue aot_box_str(const char *s) {")
    c_out.append("    return mx_str(s);")
    c_out.append("}")
    c_out.append("")
    c_out.append("static inline MxValue aot_box_int(int64_t i) {")
    c_out.append("    return mx_int(i);")
    c_out.append("}")
    c_out.append("")
    c_out.append("static inline MxValue aot_box_num(double d) {")
    c_out.append("    return mx_num(d);")
    c_out.append("}")
    c_out.append("")
    c_out.append("static inline MxValue aot_box_bool(int b) {")
    c_out.append("    return mx_bool(b);")
    c_out.append("}")
    c_out.append("")
    c_out.append("static inline MxValue aot_box_null(void) {")
    c_out.append("    return mx_null();")
    c_out.append("}")
    c_out.append("")
    c_out.append("static inline MxValue aot_box_arr(MxArray *items) {")
    c_out.append("    return mx_arr_value(items);")
    c_out.append("}")
    c_out.append("")

    entries = []

    for mod_name, file_path in sorted(modules_map.items()):
        with open(file_path, "r", encoding="utf-8") as fp:
            src_text = fp.read()

        # Emit embedded source string literal
        src_var = f"s_src_{mod_name}"
        c_out.append(f'/* ---- Module: {mod_name} ({file_path}) ---- */')
        c_out.append(f'static const char {src_var}[] = "{c_esc(src_text)}";')
        c_out.append("")

        ast = compile_code(src_text)
        body = ast.get("body", [])
        stmt_fn_names = []

        # Emit builder helper for each top-level statement in module
        for idx, stmt in enumerate(body):
            fn_name = f"aot_{mod_name}_stmt_{idx}"
            stmt_fn_names.append(fn_name)
            c_out.append(f"static MxValue {fn_name}(void) {{")
            s_lines = []
            var_id = [0]
            s_expr = emit_node_expr(stmt, s_lines, var_id)
            c_out.extend(s_lines)
            c_out.append(f"    return {s_expr};")
            c_out.append("}")
            c_out.append("")

        # Emit module builder function
        builder_fn = f"aot_build_{mod_name}"
        c_out.append(f"static MxValue {builder_fn}(void) {{")
        c_out.append("    static MxValue cache = {0};")
        c_out.append("    if (cache.tag != MX_NULL) return cache;")
        c_out.append(f'    MxValue prog = aot_mk_node("Program", 1);')
        c_out.append(f"    MxArray *body_arr = mx_arr_new({len(body)}LL);")
        for idx, fn_name in enumerate(stmt_fn_names):
            c_out.append(f"    body_arr->v[{idx}] = {fn_name}();")
        c_out.append('    aot_set_field(prog, "body", aot_box_arr(body_arr));')
        c_out.append("    cache = prog;")
        c_out.append("    return prog;")
        c_out.append("}")
        c_out.append("")

        entries.append((mod_name, src_var, builder_fn))

    c_out.append("typedef struct {")
    c_out.append("    const char *name;")
    c_out.append("    const char *source;")
    c_out.append("    MxValue (*build_prog)(void);")
    c_out.append("} MxAotEntry;")
    c_out.append("")
    c_out.append("static const MxAotEntry g_aot_entries[] = {")
    for mod_name, src_var, builder_fn in entries:
        c_out.append(f'    {{ "{mod_name}", {src_var}, {builder_fn} }},')
    c_out.append("};")
    c_out.append("")
    c_out.append("const char *mx_aot_get_source(const char *name) {")
    c_out.append("    if (!name) return NULL;")
    c_out.append("    size_t count = sizeof(g_aot_entries) / sizeof(g_aot_entries[0]);")
    c_out.append("    for (size_t i = 0; i < count; i++) {")
    c_out.append("        if (strcmp(g_aot_entries[i].name, name) == 0)")
    c_out.append("            return g_aot_entries[i].source;")
    c_out.append("    }")
    c_out.append("    return NULL;")
    c_out.append("}")
    c_out.append("")
    c_out.append("MxValue mx_aot_get_prog(const char *name) {")
    c_out.append("    if (!name) return mx_null();")
    c_out.append("    size_t count = sizeof(g_aot_entries) / sizeof(g_aot_entries[0]);")
    c_out.append("    for (size_t i = 0; i < count; i++) {")
    c_out.append("        if (strcmp(g_aot_entries[i].name, name) == 0)")
    c_out.append("            return g_aot_entries[i].build_prog();")
    c_out.append("    }")
    c_out.append("    return mx_null();")
    c_out.append("}")
    c_out.append("")

    with open(c_path, "w", encoding="utf-8") as f:
        f.write("\n".join(c_out))
    print(f"Wrote {c_path} ({len(c_out)} lines).")

if __name__ == "__main__":
    main()
