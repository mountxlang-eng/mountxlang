/* Copyright (C) 2026-2027 Ziad Mohsin. All Rights Reserved.
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
