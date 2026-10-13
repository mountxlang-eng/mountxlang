# Copyright (C) 2026-2027 Ziad Mohsin. All Rights Reserved.
import os
import sys

from lexer import lex
from parser import parse
from interpreter import (Interpreter, VERSION, ThrowValue, ReturnException,
                         BreakException, ContinueException, display)

sys.setrecursionlimit(10000)

USAGE = f"""MountX {VERSION} - a small dynamically-typed language
MountX v1.0 by Ziad Mohsin 
Usage:
  mountx                    start the REPL (state persists; exit/quit or Ctrl-D)
  mountx <file.mx>          run a program
  mountx -e "<code>"        run a snippet inline
  mountx --version, -v      print the version and exit
  mountx --help, -h         print this help and exit

Every program is wrapped in mountx ... unmountx; -e snippets are wrapped for
you. Comments start with #. Modules are imported with importx <name> and are
searched in $MOUNTX_STD, the script's directory, the cwd, then the
interpreter's own directory.
"""


def compile_code(code):
    """Lex + parse a program, auto-wrapping in mountx/unmountx when missing
    (same convention the REPL and handle_import already use)."""
    tokens = lex(code)
    if not tokens or tokens[0].type != "MOUNTX":
        tokens = lex("mountx\n" + code + "\nunmountx")
    return parse(tokens)


def get_source_line(filepath, lineno):
    if not filepath or lineno <= 0:
        return None
    try:
        if not os.path.isfile(filepath):
            return None
        with open(filepath, "r", encoding="utf-8", errors="replace") as f:
            for idx, line in enumerate(f, 1):
                if idx == lineno:
                    return line.rstrip("\r\n")
    except Exception:
        return None
    return None


def normalize_display_file(file):
    if not file:
        return ""
    try:
        norm = file.replace("\\", "/")
        idx = norm.find("/stdlib/")
        if idx != -1:
            return norm[idx + 1:]
        rel = os.path.relpath(file)
        if not rel.startswith(".."):
            return rel
        return file
    except Exception:
        return file


def format_error(e):
    """One error formatter shared by the file, -e and REPL code paths."""
    if isinstance(e, SyntaxError):
        msg = str(e)
        # the lexer and parser already label their own diagnostics
        return msg if msg.startswith("[") else f"[Syntax] {msg}"

    line = getattr(e, "line", 0)
    file = getattr(e, "file", "")
    func = getattr(e, "func", "<main>")
    call_stack = getattr(e, "call_stack", None) or []
    loc = ""
    is_docs_verify = (
        (file and os.path.basename(file).startswith("_docs_verify")) or
        any(f.get("file", "") and os.path.basename(f["file"]).startswith("_docs_verify") for f in call_stack)
    )

    if line:
        if file and not os.path.basename(file).startswith("_docs_verify"):
            display_file = normalize_display_file(file)
            loc = f"Line {line} in {display_file}: "
        else:
            loc = f"Line {line}: "

    if isinstance(e, ThrowValue):
        msg = f"[Runtime] {loc}Uncaught throwx: {display(e.value)}"
    elif isinstance(e, ReturnException):
        msg = f"[Runtime] {loc}'return' used outside a function"
    elif isinstance(e, (BreakException, ContinueException)):
        msg = f"[Runtime] {loc}'break' or 'continue' used outside a loop"
    elif isinstance(e, RuntimeError):
        msg = f"[Runtime] {loc}{e}"
    else:
        msg = f"[Runtime] {loc}{e.__class__.__name__}: {e}"

    if not file or is_docs_verify:
        return msg

    traceback_lines = []
    if call_stack:
        traceback_lines.append("Traceback (most recent call last):")
        for frame in call_stack:
            f_file = normalize_display_file(frame["file"])
            f_line = frame["line"]
            f_caller = frame["caller"]
            traceback_lines.append(f'  File "{f_file}", line {f_line}, in {f_caller}')
            srcline = get_source_line(frame["file"], f_line)
            if srcline is not None and srcline.strip():
                traceback_lines.append(f'    {srcline.strip()}')

        err_file = normalize_display_file(file)
        traceback_lines.append(f'  File "{err_file}", line {line}, in {func}')
        srcline = get_source_line(file, line)
        if srcline is not None and srcline.strip():
            traceback_lines.append(f'    {srcline.strip()}')
            traceback_lines.append('    ^')
    elif line > 0:
        err_file = normalize_display_file(file)
        traceback_lines.append(f'  File "{err_file}", line {line}, in <main>')
        srcline = get_source_line(file, line)
        if srcline is not None and srcline.strip():
            traceback_lines.append(f'    {srcline.strip()}')
            traceback_lines.append('    ^')

    if traceback_lines:
        return "\n".join(traceback_lines) + "\n" + msg
    return msg


def die(e):
    """Report one error and stop (exit 1). SystemExit passes through."""
    print(format_error(e))
    sys.exit(1)


def run_ast(interp, ast):
    try:
        interp.run(ast)
    except Exception as e:
        die(e)


def run_file(path):
    try:
        with open(path, "r", encoding="utf-8") as f:
            code = f.read()
    except OSError as e:
        print(f"[Runtime] Cannot read '{path}': {e.strerror or e}")
        sys.exit(1)
    interp = Interpreter()
    # imports resolve relative to the program's own directory first
    interp.script_dir = os.path.dirname(os.path.abspath(path))
    interp.current_file = path
    try:
        ast = compile_code(code)
    except Exception as e:
        die(e)
    run_ast(interp, ast)


def run_inline(code):
    interp = Interpreter()
    try:
        ast = compile_code(code)
    except Exception as e:
        die(e)
    run_ast(interp, ast)


def run_line(interp, ast):
    """Like Interpreter.run, but returns the value of a trailing expression
    statement so the REPL can echo it (assignments/imports stay silent)."""
    body = ast["body"]
    for stmt in body:
        if stmt["type"] == "ClassDef":
            interp.classes[stmt["name"]] = stmt
        elif stmt["type"] == "FuncDef":
            interp.globals.define(stmt["name"], stmt)
    result = None
    for i, stmt in enumerate(body):
        if stmt["type"] in ("ClassDef", "FuncDef"):
            continue
        if i == len(body) - 1 and stmt["type"] == "ExprStmt":
            result = interp.evaluate(stmt["expr"])
        else:
            interp.execute(stmt)
    return result


def repl():
    print("MountX REPL - try: importx stdlib   (exit/quit or Ctrl-D to leave)")
    interp = Interpreter()
    buf = ""
    while True:
        try:
            line = input("mountx> " if not buf else ".......> ")
        except EOFError:
            print()
            break
        # tolerate pasted prompts from docs/examples: strip "mountx> " prefixes
        for pfx in ("mountx> ", "mountx>", ".......> ", ".......>"):
            if line.startswith(pfx):
                line = line[len(pfx):]
                break
        if not buf:
            stripped = line.strip()
            if stripped in ("exit", "quit"):
                break
            if stripped == "":
                continue
        buf += line + "\n"
        if buf.count("{") > buf.count("}"):   # keep reading multi-line blocks
            continue
        try:
            ast = compile_code(buf)
        except Exception as e:
            print(format_error(e))
            buf = ""
            continue
        buf = ""
        try:
            result = run_line(interp, ast)
            if result is not None:
                print(display(result))
        except Exception as e:
            print(format_error(e))


def main(argv):
    if len(argv) < 2:
        repl()
        return 0
    flag = argv[1]
    if flag in ("--version", "-v"):
        print(f"MountX {VERSION}  (c) 2026 Ziad Mohsin")
        return 0
    if flag in ("--help", "-h"):
        print(USAGE)
        return 0
    if flag == "-e":
        if len(argv) < 3:
            print("[Runtime] -e expects a code string")
            return 1
        run_inline(argv[2])
        return 0
    if flag.startswith("-") and flag != "-":
        print(f"[Runtime] Unknown option '{flag}'\n")
        print(USAGE)
        return 1
    run_file(flag)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
