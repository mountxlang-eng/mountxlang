# Copyright (C) 2026-2027 Ziad Mohsin. All Rights Reserved.
"""
Interpreter for the mountx language.
Pairs with lexer.py + parser.py

Session fixes integrated:
  1. eval_binop: ==/!=/relational ops handled before the numeric guard,
     via values_equal (deep array equality, instance identity, bool!=int).
  2. execute(): ClassDef allowed inside blocks (classes register in self.classes).
  3. eval_dot: method lookup goes through find_method, so 'init' (stored in
     its own class slot) is reachable via super.init / this.init / obj.init.
  4. handle_import: searches category subfolders (core/, data/, structs/,
     text/, math/, system/) and re-exports transitive imports, so bundle
     modules (collections.mx) work and dotted access (ds.Stack) resolves.
  5. eval_call: native builtins for the stdlib - time_now_ms, sleep_ms,
     sys_args, sys_exit, os_env, os_cwd, os_listdir, os_sep, mx_version,
     http_get, plus the db_* family.
  6. handle_import: searches $MOUNTX_STD, the entry script's directory, the
     cwd, and this file's directory, so imports resolve from any cwd.
"""

# Language version - single source of truth for `mountx --version` and the
# mx_version() native. Bump this for any user-visible change (see AGENTS.md).
VERSION = "1.1.0"


class ReturnException(Exception):
    def __init__(self, value):
        self.value = value


class BreakException(Exception):
    pass


class ContinueException(Exception):
    pass


class ThrowValue(Exception):
    """Carries the value from throwx so catchx receives it unchanged
    (not the string form of a Python error)."""
    def __init__(self, value):
        self.value = value
        super().__init__(value if isinstance(value, str) else None)


# Control-flow signals must never be caught by tryx/catchx
CONTROL_FLOW = (ReturnException, BreakException, ContinueException)


def is_num(v):
    return isinstance(v, (int, float)) and not isinstance(v, bool)


def display(v, quote_strings=False, _seen=None):
    """Human-readable form of a value (printx, string concat, file writes).
    true/false/null print as language values, not Python's True/False/None."""
    if v is None:
        return "null"
    if v is True:
        return "true"
    if v is False:
        return "false"
    if isinstance(v, str):
        return '"' + v + '"' if quote_strings else v
    if isinstance(v, list):
        if _seen is None:
            _seen = set()
        if id(v) in _seen:
            return "[...]"
        _seen = _seen | {id(v)}
        return "[" + ", ".join(display(x, True, _seen) for x in v) + "]"
    if isinstance(v, MxInstance):
        if _seen is None:
            _seen = set()
        if id(v) in _seen:
            return "{...}"
        _seen = _seen | {id(v)}
        inner = ", ".join(f"{k}: {display(x, True, _seen)}" for k, x in v.fields.items())
        return f"{v.klass['name']}{{{inner}}}"
    if isinstance(v, MxModule):
        return f"<module {v.mod_name}>"
    if isinstance(v, dict):
        if v.get("type") == "FuncDef":
            return f"<func {v['name']}>"
        if v.get("type") == "BoundMethod":
            return f"<method {v['method']['name']}>"
    return str(v)


def type_name(v):
    """Language type names for typeofx ('string', not Python's 'str')."""
    if v is None:
        return "null"
    if isinstance(v, bool):
        return "bool"
    if isinstance(v, int):
        return "int"
    if isinstance(v, float):
        return "float"
    if isinstance(v, str):
        return "string"
    if isinstance(v, list):
        return "array"
    if isinstance(v, MxInstance):
        return v.klass["name"]
    if isinstance(v, MxModule):
        return "module"
    if isinstance(v, dict) and v.get("type") in ("FuncDef", "BoundMethod"):
        return "function"
    return type(v).__name__


class Environment:
    __slots__ = ("values", "parent")

    def __init__(self, parent=None):
        self.values = {}
        self.parent = parent

    def define(self, name, value):
        self.values[name] = value

    def get(self, name):
        env = self
        while env is not None:
            if name in env.values:
                return env.values[name]
            env = env.parent
        raise RuntimeError(f"Undefined variable '{name}'")

    def set(self, name, value):
        env = self
        while env is not None:
            if name in env.values:
                env.values[name] = value
                return
            env = env.parent
        self.values[name] = value   # assigning an undeclared name declares it

    def exists_in_chain(self, name):
        env = self
        while env is not None:
            if name in env.values:
                return True
            env = env.parent
        return False


class MxInstance:
    def __init__(self, klass, fields):
        self.klass = klass
        self.fields = fields

    def __repr__(self):
        return display(self)


class MxModule:
    def __init__(self, mod_name, funcs, classes, lets=None):
        self.mod_name = mod_name
        self.funcs = funcs
        self.classes = classes
        self.lets = lets if lets is not None else {}

    def __repr__(self):
        return display(self)


class Interpreter:
    def __init__(self):
        self.globals = Environment()
        self.env = self.globals
        self.classes = {}
        self._imported = set()
        # Directory of the entry script, if any. Set by main.py before run so
        # imports resolve relative to the program being run, not the cwd.
        self.script_dir = None
        self.current_file = ""
        self.current_line = 0
        self.current_func = "<main>"
        self.call_stack = []
        self._db_conns = {}
        self._db_next_id = 1
        self._tcp_sockets = {}
        self._tcp_next_id = 1

    # ------------------------------------------------------------------
    # imports (subfolder-aware + re-exports transitive imports)
    # ------------------------------------------------------------------
    def handle_import(self, name):
        if name in self._imported:
            return
        self._imported.add(name)
        import os
        # flat stdlib first, then the category folders (core/, data/, ...)
        subs = ["", "core", "data", "structs", "text", "math", "system"]
        # Search bases, first hit wins: $MOUNTX_STD (if set), the entry
        # script's directory, the cwd, then this file's directory. Each base is
        # tried for stdlib/<sub>/<name>.mx, stdlib/<name>.mx, libs/<name>.mx
        # and <name>.mx; duplicate paths are dropped, order is preserved.
        bases = []
        env_std = os.environ.get("MOUNTX_STD")
        if env_std:
            bases.append(env_std)
        if self.script_dir:
            bases.append(self.script_dir)
        bases.append(os.getcwd())
        bases.append(os.path.dirname(os.path.abspath(__file__)))

        def candidates(base):
            for root in ("stdlib", "libs", ""):
                for sub in subs:
                    parts = [base] if root == "" else [base, root]
                    parts = parts + ([sub, f"{name}.mx"] if sub else [f"{name}.mx"])
                    yield os.path.normpath(os.path.join(*parts))

        search = []
        seen = set()
        for base in bases:
            for p in candidates(base):
                if p not in seen:
                    seen.add(p)
                    search.append(p)
        found = None
        for p in search:
            if os.path.exists(p):
                found = p
                break
        if not found:
            raise RuntimeError(f"Module '{name}' not found. Searched: {search}")

        from lexer import lex
        from parser import parse
        with open(found, "r", encoding="utf-8") as f:
            file_code = f.read()

        tokens = lex(file_code)
        if not tokens or tokens[0].type != "MOUNTX":
            tokens = lex("mountx\n" + file_code + "\nunmountx")
        ast = parse(tokens)

        module_funcs = {}
        module_classes = {}
        module_lets = {}
        # Module top level runs in its OWN scope: lets land here (NOT in
        # globals), and every module func/method gets this env chained under
        # its calls, so bare references to module lets still resolve.
        mod_scope = Environment(self.globals)
        prev = self.env
        prev_file = self.current_file
        self.env = mod_scope
        self.current_file = found
        try:
            for stmt in ast["body"]:
                st = stmt["type"]
                if st == "ClassDef":
                    stmt["__file__"] = found
                    self.classes[stmt["name"]] = stmt
                    module_classes[stmt["name"]] = stmt
                    for m in stmt.get("methods", []):
                        m["__mod_env__"] = mod_scope
                        m["__file__"] = found
                    if stmt.get("init"):
                        stmt["init"]["__mod_env__"] = mod_scope
                        stmt["init"]["__file__"] = found
                elif st == "FuncDef":
                    stmt["__mod_env__"] = mod_scope
                    stmt["__file__"] = found
                    existing = self.globals.values.get(stmt["name"])
                    if not isinstance(existing, MxModule):
                        self.globals.define(stmt["name"], stmt)
                    module_funcs[stmt["name"]] = stmt
                elif st == "Import":
                    self.handle_import(stmt["name"])
                    try:
                        sub = self.globals.get(stmt["name"])
                    except RuntimeError:
                        sub = None
                    if isinstance(sub, MxModule):
                        for fname, fdef in sub.funcs.items():
                            module_funcs.setdefault(fname, fdef)
                        for cname, cdef in sub.classes.items():
                            module_classes.setdefault(cname, cdef)
                elif st == "Let":
                    val = self.evaluate(stmt["value"])
                    mod_scope.define(stmt["name"], val)
                    module_lets[stmt["name"]] = val
        finally:
            self.env = prev
            self.current_file = prev_file

        if module_funcs or module_classes or module_lets:
            self.globals.define(name, MxModule(name, module_funcs, module_classes, module_lets))

    # ------------------------------------------------------------------
    # program
    # ------------------------------------------------------------------
    def run(self, ast):
        if ast["type"] != "Program":
            raise RuntimeError("Expected Program node")
        if ast.get("line"):
            self.current_line = ast["line"]
        for stmt in ast["body"]:          # hoist classes & functions
            if stmt["type"] == "ClassDef":
                self.classes[stmt["name"]] = stmt
            elif stmt["type"] == "FuncDef":
                self.globals.define(stmt["name"], stmt)
        for stmt in ast["body"]:
            if stmt["type"] in ("ClassDef", "FuncDef"):
                continue
            self.execute(stmt)

    # ------------------------------------------------------------------
    # statements
    # ------------------------------------------------------------------
    def execute(self, node):
        prev_line = self.current_line
        if isinstance(node, dict) and node.get("line"):
            self.current_line = node["line"]
        try:
            t = node["type"]
            if t == "Let":
                self.env.define(node["name"], self.evaluate(node["value"]))
            elif t == "Assign":
                val = self.evaluate(node["value"])
                self.assign_target(node["target"], val)
            elif t == "Print":
                print(display(self.evaluate(node["value"])))
            elif t == "ExprStmt":
                self.evaluate(node["expr"])
            elif t == "If":
                if self.is_truthy(self.evaluate(node["condition"])):
                    self.execute_block(node["then"])
                elif node["else"]:
                    self.execute_block(node["else"])
            elif t == "While":
                while self.is_truthy(self.evaluate(node["condition"])):
                    try:
                        self.execute_block(node["body"])
                    except BreakException:
                        break
                    except ContinueException:
                        continue
            elif t == "Loop":
                count = self.evaluate(node["count"])
                if isinstance(count, bool) or not isinstance(count, int):
                    raise RuntimeError("loop count must be an integer")
                for _ in range(max(0, count)):
                    try:
                        self.execute_block(node["body"])
                    except BreakException:
                        break
                    except ContinueException:
                        continue
            elif t == "ForIn":
                self.exec_forin(node)
            elif t == "FuncDef":
                self.env.define(node["name"], node)
            elif t == "ClassDef":
                self.classes[node["name"]] = node
            elif t == "Import":
                self.handle_import(node["name"])
            elif t == "Return":
                val = self.evaluate(node["value"]) if node["value"] is not None else None
                exc = ReturnException(val)
                exc.line = node.get("line") or self.current_line
                exc.file = self.current_file
                raise exc
            elif t == "Break":
                exc = BreakException()
                exc.line = node.get("line") or self.current_line
                exc.file = self.current_file
                raise exc
            elif t == "Continue":
                exc = ContinueException()
                exc.line = node.get("line") or self.current_line
                exc.file = self.current_file
                raise exc
            elif t == "TryCatch":
                try:
                    self.execute_block(node["try_body"])
                except CONTROL_FLOW:
                    raise                 # never swallow return/break/continue
                except ThrowValue as tv:  # thrown values pass through unchanged
                    self.run_catch(node, tv.value)
                except Exception as e:    # runtime errors become catchable messages
                    self.run_catch(node, str(e) or e.__class__.__name__)
            elif t == "Throw":
                tv = ThrowValue(self.evaluate(node["value"]))
                tv.line = node.get("line") or self.current_line
                tv.file = self.current_file
                tv.func = self.current_func
                tv.call_stack = list(self.call_stack)
                raise tv
            else:
                raise RuntimeError(f"Unknown stmt {t}")
        except RuntimeError as e:
            if not hasattr(e, "line"):
                e.line = self.current_line
            if not hasattr(e, "file"):
                e.file = self.current_file
            if not hasattr(e, "func"):
                e.func = self.current_func
            if not hasattr(e, "call_stack"):
                e.call_stack = list(self.call_stack)
            raise
        finally:
            self.current_line = prev_line

    def run_catch(self, node, caught_value):
        catch_env = Environment(self.env)
        prev = self.env
        self.env = catch_env
        self.env.define(node["catch_var"], caught_value)
        try:
            self.execute_block(node["catch_body"])
        finally:
            self.env = prev

    def execute_block(self, stmts):
        block_env = Environment(self.env)
        prev = self.env
        self.env = block_env
        try:
            for s in stmts:
                self.execute(s)
        finally:
            self.env = prev

    def exec_forin(self, node):
        iterable = self.evaluate(node["iterable"])
        if not isinstance(iterable, (list, str)):
            raise RuntimeError("for...in expects an array or string")
        for idx, elem in enumerate(iterable):
            iter_env = Environment(self.env)
            if node.get("var2"):
                iter_env.define(node["var"], idx)
                iter_env.define(node["var2"], elem)
            else:
                iter_env.define(node["var"], elem)
            prev = self.env
            self.env = iter_env
            try:
                self.execute_block(node["body"])
            except BreakException:
                break
            except ContinueException:
                continue
            finally:
                self.env = prev

    def assign_target(self, target, value):
        tt = target["type"]
        if tt == "Ident":
            self.env.set(target["name"], value)
        elif tt == "DotAccess":
            obj = self.evaluate(target["object"])
            if isinstance(obj, MxInstance):
                obj.fields[target["field"]] = value
            else:
                raise RuntimeError("Cannot assign to field of non-instance")
        elif tt == "Index":
            arr = self.evaluate(target["array"])
            idx = self.evaluate(target["index"])
            if not isinstance(arr, list):
                raise RuntimeError("Index assignment target is not an array")
            if isinstance(idx, bool) or not isinstance(idx, int):
                raise RuntimeError("Array index must be an integer")
            if idx < 0 or idx >= len(arr):
                raise RuntimeError(f"Index {idx} out of range (length {len(arr)})")
            arr[idx] = value
        else:
            raise RuntimeError(f"Bad assign target {tt}")

    # ------------------------------------------------------------------
    # class resolution (supports dotted parents like dict.Dict)
    # ------------------------------------------------------------------
    def resolve_class(self, name):
        """Resolve a class name to its AST node. Accepts plain names
        ('Dict') and dotted module names ('dict.Dict'). None if not found.
        Plain names check the global registry first, so a local class
        shadows a module class of the same name."""
        if name in self.classes:
            return self.classes[name]
        if "." in name:
            mod_name, cls_name = name.rsplit(".", 1)
            try:
                mod = self.globals.get(mod_name)
            except RuntimeError:
                return None
            if isinstance(mod, MxModule) and cls_name in mod.classes:
                return mod.classes[cls_name]
        return None

    def class_chain(self, klass):
        """Inheritance chain [klass, parent, grandparent, ...].
        Raises on unknown parents and inheritance cycles."""
        chain = []
        seen = set()
        cur = klass
        while cur is not None:
            if id(cur) in seen:
                raise RuntimeError(f"Inheritance cycle involving class '{cur['name']}'")
            seen.add(id(cur))
            chain.append(cur)
            pname = cur.get("parent")
            if not pname:
                break
            cur = self.resolve_class(pname)
            if cur is None:
                raise RuntimeError(f"Class '{chain[-1]['name']}' extends unknown class '{pname}'")
        return chain

    # ------------------------------------------------------------------
    # expressions
    # ------------------------------------------------------------------
    def evaluate(self, node):
        prev_line = self.current_line
        if isinstance(node, dict) and node.get("line"):
            self.current_line = node["line"]
        try:
            t = node["type"]
            if t == "Literal":
                return node["value"]
            if t == "Ident":
                return self.env.get(node["name"])
            if t == "This":
                if not self.env.exists_in_chain("this"):
                    raise RuntimeError("'this' can only be used inside a method or init")
                return self.env.get("this")
            if t == "Super":
                if not self.env.exists_in_chain("super"):
                    raise RuntimeError("'super' can only be used inside a method of a class with a parent")
                sup = self.env.get("super")
                if sup is None:
                    raise RuntimeError("'super' used but this class has no parent")
                return sup
            if t == "ArrayLiteral":
                return [self.evaluate(e) for e in node["elements"]]
            if t == "BinOp":
                op = node["op"]
                left = self.evaluate(node["left"])
                if op == "AND":
                    if not self.is_truthy(left):
                        return False
                    return self.is_truthy(self.evaluate(node["right"]))
                if op == "OR":
                    if self.is_truthy(left):
                        return True
                    return self.is_truthy(self.evaluate(node["right"]))
                return self.eval_binop(op, left, self.evaluate(node["right"]))
            if t == "UnaryOp":
                v = self.evaluate(node["value"])
                if node["op"] == "MINUS":
                    if not is_num(v):
                        raise RuntimeError("Unary '-' needs a number")
                    return -v
                if node["op"] == "NOT":
                    return not self.is_truthy(v)
                raise RuntimeError(f"Unknown unary {node['op']}")
            if t == "Call":
                args = [self.evaluate(a) for a in node["args"]]
                return self.eval_call(node["callee"], args)
            if t == "DotAccess":
                return self.eval_dot(node)
            if t == "Index":
                return self.eval_index(node)
            if t == "Input":
                prompt = ""
                if node.get("prompt") is not None:
                    prompt = display(self.evaluate(node["prompt"]))
                return input(prompt)
            if t == "TypeOf":
                return type_name(self.evaluate(node["value"]))
            if t == "New":
                return self.eval_new(node)
            if t in ("Read", "Write", "Append", "Export"):
                return self.eval_file_builtin(t, node)
            raise RuntimeError(f"Unknown expr type: {t}")
        except RuntimeError as e:
            if not hasattr(e, "line"):
                e.line = self.current_line
            if not hasattr(e, "file"):
                e.file = self.current_file
            if not hasattr(e, "func"):
                e.func = self.current_func
            if not hasattr(e, "call_stack"):
                e.call_stack = list(self.call_stack)
            raise
        finally:
            self.current_line = prev_line

    def values_equal(self, a, b):
        """Equality for == and !=. Works on any types; cross-type compares are False."""
        # bools are ints in Python - keep them distinct from numbers
        if isinstance(a, bool) or isinstance(b, bool):
            return isinstance(a, bool) and isinstance(b, bool) and a == b
        if a is None or b is None:
            return a is None and b is None
        if is_num(a) and is_num(b):
            return a == b
        if isinstance(a, str) and isinstance(b, str):
            return a == b
        if isinstance(a, list) and isinstance(b, list):
            return (len(a) == len(b)
                    and all(self.values_equal(x, y) for x, y in zip(a, b)))
        return a is b   # instances, functions, modules: identity

    def eval_binop(self, op, left, right):
        if op == "PLUS":
            if isinstance(left, str) or isinstance(right, str):
                return display(left) + display(right)
            if isinstance(left, list) and isinstance(right, list):
                return left + right
            if is_num(left) and is_num(right):
                return left + right
            raise RuntimeError(f"Cannot add {type_name(left)} and {type_name(right)}")
        if op == "EQEQ":
            return self.values_equal(left, right)
        if op == "NEQ":
            return not self.values_equal(left, right)
        if op in ("GT", "LT", "GTE", "LTE"):
            if not ((is_num(left) and is_num(right)) or
                    (isinstance(left, str) and isinstance(right, str))):
                raise RuntimeError(f"Cannot compare {type_name(left)} and {type_name(right)}")
            if op == "GT":  return left > right
            if op == "LT":  return left < right
            if op == "GTE": return left >= right
            return left <= right
        if not (is_num(left) and is_num(right)):
            raise RuntimeError(f"'{op}' needs numbers, got {type_name(left)} and {type_name(right)}")
        if op == "MINUS":
            return left - right
        if op == "MUL":
            return left * right
        if op == "DIV":
            if right == 0:
                raise RuntimeError("Division by zero")
            return left / right
        if op == "MOD":
            if right == 0:
                raise RuntimeError("Modulo by zero")
            return left % right
        raise RuntimeError(f"Unknown binop {op}")

    def find_method(self, klass, name):
        """Find a method by name in a single class. 'init' lives in its own
        slot (not the methods list), so check it explicitly."""
        if name == "init" and klass.get("init"):
            return klass["init"]
        for m in klass.get("methods", []):
            if m["name"] == name:
                return m
        return None

    def eval_dot(self, node):
        obj = self.evaluate(node["object"])
        field = node["field"]
        if isinstance(obj, MxModule):
            if field in obj.funcs:
                return obj.funcs[field]
            if field in obj.classes:
                return obj.classes[field]
            if field in obj.lets:
                return obj.lets[field]
            raise RuntimeError(f"Module '{obj.mod_name}' has no attribute '{field}'")
        if isinstance(obj, MxInstance):
            if field in obj.fields:
                return obj.fields[field]
            for k in self.class_chain(obj.klass):
                m = self.find_method(k, field)
                if m is not None:
                    return {"type": "BoundMethod", "method": m,
                            "instance": obj, "klass": k}
            raise RuntimeError(f"Field/method '{field}' not found on {obj.klass['name']}")
        if isinstance(obj, dict) and obj.get("__is_super__"):
            for k in self.class_chain(obj["klass"]):
                m = self.find_method(k, field)
                if m is not None:
                    return {"type": "BoundMethod", "method": m,
                            "instance": obj["instance"], "klass": k}
            raise RuntimeError(f"super has no method '{field}'")
        raise RuntimeError(f"Cannot access field '{field}' on {type_name(obj)}")

    def eval_index(self, node):
        arr = self.evaluate(node["array"])
        idx = self.evaluate(node["index"])
        if not isinstance(arr, (list, str)):
            raise RuntimeError(f"Cannot index a {type_name(arr)}")
        if isinstance(idx, bool) or not isinstance(idx, int):
            raise RuntimeError("Index must be an integer")
        if idx < 0 or idx >= len(arr):
            raise RuntimeError(f"Index {idx} out of range (length {len(arr)})")
        return arr[idx]

    def eval_new(self, node):
        cname = node["class"]
        if "." in cname:
            # newx module.Class(...)
            mod_name, cls_name = cname.rsplit(".", 1)
            mod = self.globals.get(mod_name)
            if not isinstance(mod, MxModule) or cls_name not in mod.classes:
                raise RuntimeError(f"Module '{mod_name}' has no class '{cls_name}'")
            klass = mod.classes[cls_name]
        else:
            klass = self.resolve_class(cname)
            if klass is None:
                raise RuntimeError(f"Class '{cname}' not defined")
        args = [self.evaluate(a) for a in node["args"]]

        chain = self.class_chain(klass)   # validates parents + cycles

        fields = {}
        for k in reversed(chain):         # parent fields first, child overrides
            for f in k.get("fields", []):
                fields[f["name"]] = self.evaluate(f["value"])
        instance = MxInstance(klass, fields)

        # nearest init up the chain (inherited init runs if the class has none)
        init_class = None
        for k in chain:
            if k.get("init"):
                init_class = k
                break
        if init_class:
            self.call_function(init_class["init"], args, instance, init_class)
        elif args:
            raise RuntimeError(f"Class '{klass['name']}' has no init but "
                               f"{len(args)} argument(s) were given")
        return instance

    def eval_call(self, callee_node, args):
        ct = callee_node["type"]
        if ct == "Ident":
            name = callee_node["name"]
            if name == "len":
                if len(args) != 1:
                    raise RuntimeError("len expects 1 argument")
                if not isinstance(args[0], (list, str)):
                    raise RuntimeError(f"len expects a string or array, got {type_name(args[0])}")
                return len(args[0])

            # ---- stdlib natives (time / sys / os / net) ----
            if name == "time_now_ms":
                if len(args) != 0:
                    raise RuntimeError("time_now_ms expects 0 arguments")
                import time as _time
                return int(_time.time() * 1000)

            if name == "sleep_ms":
                if len(args) != 1 or not is_num(args[0]):
                    raise RuntimeError("sleep_ms expects 1 number of milliseconds")
                import time as _time
                _time.sleep(max(0.0, args[0] / 1000.0))
                return None

            if name == "sys_args":
                if len(args) != 0:
                    raise RuntimeError("sys_args expects 0 arguments")
                import sys as _sys
                return _sys.argv[2:]   # args after: python3 main.py prog.mx

            if name == "sys_exit":
                if len(args) != 1 or not is_num(args[0]):
                    raise RuntimeError("sys_exit expects 1 numeric exit code")
                import sys as _sys
                _sys.exit(int(args[0]))

            if name == "sys_exec":
                if len(args) != 1 or not isinstance(args[0], str):
                    raise RuntimeError("sys_exec expects 1 command string")
                import subprocess as _subprocess
                try:
                    res = _subprocess.run(args[0], shell=True, stdout=_subprocess.PIPE, stderr=_subprocess.STDOUT, text=True)
                    return [res.returncode, res.stdout]
                except Exception as e:
                    raise RuntimeError(f"sys_exec failed: {e}")

            if name == "str_ord":
                if len(args) != 1 or not isinstance(args[0], str) or len(args[0]) != 1:
                    raise RuntimeError("str_ord expects 1 single-character string")
                return ord(args[0])

            if name == "str_chr":
                if len(args) != 1 or not is_num(args[0]):
                    raise RuntimeError("str_chr expects 1 integer")
                code = int(args[0])
                if code < 0 or code > 255:
                    raise RuntimeError("str_chr expects an integer in range 0..255")
                return chr(code)

            if name == "os_env":
                if len(args) != 1 or not isinstance(args[0], str):
                    raise RuntimeError("os_env expects 1 string argument")
                import os as _os
                return _os.environ.get(args[0])

            if name == "os_cwd":
                if len(args) != 0:
                    raise RuntimeError("os_cwd expects 0 arguments")
                import os as _os
                return _os.getcwd()

            if name == "os_listdir":
                if len(args) != 1 or not isinstance(args[0], str):
                    raise RuntimeError("os_listdir expects 1 string argument")
                import os as _os
                try:
                    return sorted(_os.listdir(args[0]))
                except OSError as e:
                    raise RuntimeError(f"os_listdir: {e}")

            if name == "os_sep":
                if len(args) != 0:
                    raise RuntimeError("os_sep expects 0 arguments")
                import os as _os
                return _os.sep

            if name == "mx_version":
                if len(args) != 0:
                    raise RuntimeError("mx_version expects 0 arguments")
                return VERSION

            if name == "math_floor":
                if len(args) != 1 or not is_num(args[0]):
                    raise RuntimeError("math_floor expects 1 number")
                import math as _math
                return _math.floor(args[0])

            if name == "idiv":
                if len(args) != 2 or not is_num(args[0]) or not is_num(args[1]):
                    raise RuntimeError("idiv expects 2 numbers")
                if args[1] == 0:
                    raise RuntimeError("idiv: division by zero")
                q = abs(args[0]) // abs(args[1])
                if (args[0] < 0) != (args[1] < 0):
                    q = -q
                return q

            if name == "db_open":
                if len(args) != 1 or not isinstance(args[0], str):
                    raise RuntimeError("db_open expects 1 string path")
                import sqlite3 as _sqlite3
                try:
                    conn = _sqlite3.connect(args[0])
                    cid = self._db_next_id
                    self._db_next_id += 1
                    self._db_conns[cid] = conn
                    return cid
                except Exception as e:
                    raise RuntimeError(f"db_open failed: {e}")

            if name == "db_close":
                if len(args) != 1 or not is_num(args[0]):
                    raise RuntimeError("db_close expects 1 connection id")
                cid = int(args[0])
                conn = self._db_conns.get(cid)
                if conn is None:
                    raise RuntimeError(f"db_close: unknown connection {cid}")
                try:
                    conn.close()
                finally:
                    del self._db_conns[cid]
                return None

            if name == "db_exec":
                if len(args) < 2 or not is_num(args[0]) or not isinstance(args[1], str):
                    raise RuntimeError("db_exec expects (conn_id, sql, [params])")
                cid = int(args[0])
                conn = self._db_conns.get(cid)
                if conn is None:
                    raise RuntimeError(f"db_exec: unknown connection {cid}")
                sql = args[1]
                params = args[2] if len(args) > 2 else None
                try:
                    cur = conn.cursor()
                    if params is None:
                        cur.execute(sql)
                    else:
                        if not isinstance(params, list):
                            raise RuntimeError("db_exec params must be an array")
                        cur.execute(sql, params)
                    conn.commit()
                    return cur.rowcount
                except Exception as e:
                    raise RuntimeError(f"db_exec failed: {e}")

            if name == "db_query":
                if len(args) < 2 or not is_num(args[0]) or not isinstance(args[1], str):
                    raise RuntimeError("db_query expects (conn_id, sql, [params])")
                cid = int(args[0])
                conn = self._db_conns.get(cid)
                if conn is None:
                    raise RuntimeError(f"db_query: unknown connection {cid}")
                sql = args[1]
                params = args[2] if len(args) > 2 else None
                try:
                    cur = conn.cursor()
                    if params is None:
                        cur.execute(sql)
                    else:
                        if not isinstance(params, list):
                            raise RuntimeError("db_query params must be an array")
                        cur.execute(sql, params)
                    rows = cur.fetchall()
                    # Convert tuples to lists for MountX
                    out = []
                    for r in rows:
                        out.append(list(r))
                    return out
                except Exception as e:
                    raise RuntimeError(f"db_query failed: {e}")

            if name == "http_get":
                if len(args) != 1 or not isinstance(args[0], str):
                    raise RuntimeError("http_get expects 1 string argument")
                import urllib.request, urllib.error
                try:
                    with urllib.request.urlopen(args[0], timeout=15) as resp:
                        return resp.read().decode("utf-8", "replace")
                except (urllib.error.URLError, OSError) as e:
                    raise RuntimeError(f"http_get failed: {e}")

            if name == "http_post":
                if len(args) < 2 or len(args) > 3 or not isinstance(args[0], str) or not isinstance(args[1], str) or (len(args) == 3 and not isinstance(args[2], str)):
                    raise RuntimeError("http_post expects (url, body, [content_type])")
                import urllib.request, urllib.error
                data = args[1].encode("utf-8")
                content_type = args[2] if len(args) == 3 else "application/x-www-form-urlencoded"
                req = urllib.request.Request(args[0], data=data, method="POST", headers={"Content-Type": content_type})
                try:
                    with urllib.request.urlopen(req, timeout=15) as resp:
                        return resp.read().decode("utf-8", "replace")
                except (urllib.error.URLError, OSError) as e:
                    raise RuntimeError(f"http_post failed: {e}")

            if name == "tcp_listen":
                if len(args) < 1 or len(args) > 2 or isinstance(args[0], bool) or not isinstance(args[0], (int, float)) or (len(args) == 2 and not isinstance(args[1], str)):
                    raise RuntimeError("tcp_listen expects (port, [host])")
                port = int(args[0])
                host = args[1] if len(args) == 2 else "0.0.0.0"
                import socket
                try:
                    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
                    s.bind((host, port))
                    s.listen(128)
                    sid = self._tcp_next_id
                    self._tcp_next_id += 1
                    self._tcp_sockets[sid] = s
                    return sid
                except Exception as e:
                    raise RuntimeError(f"tcp_listen failed: {e}")

            if name == "tcp_accept":
                if len(args) != 1 or isinstance(args[0], bool) or not isinstance(args[0], (int, float)):
                    raise RuntimeError("tcp_accept expects 1 socket descriptor")
                sid = int(args[0])
                s = self._tcp_sockets.get(sid)
                if not s:
                    raise RuntimeError(f"tcp_accept: unknown or closed socket {sid}")
                try:
                    client_s, addr = s.accept()
                    csid = self._tcp_next_id
                    self._tcp_next_id += 1
                    self._tcp_sockets[csid] = client_s
                    return [csid, str(addr[0])]
                except Exception as e:
                    raise RuntimeError(f"tcp_accept failed: {e}")

            if name == "tcp_recv":
                if len(args) != 2 or isinstance(args[0], bool) or not isinstance(args[0], (int, float)) or isinstance(args[1], bool) or not isinstance(args[1], (int, float)):
                    raise RuntimeError("tcp_recv expects (fd, max_len)")
                sid = int(args[0])
                max_len = int(args[1])
                s = self._tcp_sockets.get(sid)
                if not s:
                    raise RuntimeError(f"tcp_recv: unknown or closed socket {sid}")
                try:
                    raw = s.recv(max_len)
                    return raw.decode("utf-8", "replace")
                except Exception as e:
                    raise RuntimeError(f"tcp_recv failed: {e}")

            if name == "tcp_send":
                if len(args) != 2 or isinstance(args[0], bool) or not isinstance(args[0], (int, float)) or not isinstance(args[1], str):
                    raise RuntimeError("tcp_send expects (fd, string_data)")
                sid = int(args[0])
                data = args[1].encode("utf-8")
                s = self._tcp_sockets.get(sid)
                if not s:
                    raise RuntimeError(f"tcp_send: unknown or closed socket {sid}")
                try:
                    s.sendall(data)
                    return len(data)
                except Exception as e:
                    raise RuntimeError(f"tcp_send failed: {e}")

            if name == "tcp_close":
                if len(args) != 1 or isinstance(args[0], bool) or not isinstance(args[0], (int, float)):
                    raise RuntimeError("tcp_close expects 1 socket descriptor")
                sid = int(args[0])
                s = self._tcp_sockets.pop(sid, None)
                if s:
                    try:
                        s.close()
                    except Exception:
                        pass
                return None

            if name == "tcp_connect":
                if len(args) != 2 or not isinstance(args[0], str) or isinstance(args[1], bool) or not isinstance(args[1], (int, float)):
                    raise RuntimeError("tcp_connect expects (host, port)")
                host = args[0]
                port = int(args[1])
                import socket
                try:
                    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                    s.connect((host, port))
                    sid = self._tcp_next_id
                    self._tcp_next_id += 1
                    self._tcp_sockets[sid] = s
                    return sid
                except Exception as e:
                    raise RuntimeError(f"tcp_connect failed: {e}")

            if name == "aot_get_prog":
                if len(args) != 1 or not isinstance(args[0], str):
                    raise RuntimeError("aot_get_prog expects 1 module name string")
                return None

            func = self.env.get(name)
            if isinstance(func, dict) and func.get("type") == "FuncDef":
                return self.call_function(func, args, None, None)
            if isinstance(func, dict) and func.get("type") == "BoundMethod":
                return self.call_function(func["method"], args, func["instance"], func["klass"])
            raise RuntimeError(f"'{name}' is not callable")
        if ct == "DotAccess":
            bound = self.evaluate(callee_node)
            if isinstance(bound, dict) and bound.get("type") == "BoundMethod":
                return self.call_function(bound["method"], args, bound["instance"], bound["klass"])
            if isinstance(bound, dict) and bound.get("type") == "FuncDef":
                return self.call_function(bound, args, None, None)
            raise RuntimeError(f"'{callee_node['field']}' is not callable")
        if ct == "Call":
            func_val = self.evaluate(callee_node)
            if isinstance(func_val, dict) and func_val.get("type") == "FuncDef":
                return self.call_function(func_val, args, None, None)
            if isinstance(func_val, dict) and func_val.get("type") == "BoundMethod":
                return self.call_function(func_val["method"], args,
                                          func_val["instance"], func_val["klass"])
            raise RuntimeError("Call result is not callable")
        if ct == "Super":
            # bare super(...) calls the nearest init up the chain
            sup = self.evaluate(callee_node)
            if not (isinstance(sup, dict) and sup.get("__is_super__")):
                raise RuntimeError("'super(...)' can only be used inside a method "
                                   "of a class with a parent")
            for k in self.class_chain(sup["klass"]):
                if k.get("init"):
                    return self.call_function(k["init"], args, sup["instance"], k)
            raise RuntimeError("No init found in super classes for super(...)")
        raise RuntimeError(f"Cannot call {ct}")

    def call_function(self, func_def, args, this_instance, klass):
        if len(args) != len(func_def["params"]):
            raise RuntimeError(f"Function '{func_def['name']}' expects "
                               f"{len(func_def['params'])} args, got {len(args)}")
        # module functions chain to their module's let-scope (globals beyond)
        parent = func_def.get("__mod_env__") or self.globals
        call_env = Environment(parent)
        if this_instance is not None:
            call_env.define("this", this_instance)
            if klass and klass.get("parent"):
                parent = self.resolve_class(klass["parent"])
                if parent is not None:
                    call_env.define("super", {
                        "__is_super__": True,
                        "instance": this_instance,
                        "klass": parent,
                    })
        for name, val in zip(func_def["params"], args):
            call_env.define(name, val)
        prev = self.env
        prev_file = self.current_file
        prev_func = self.current_func
        self.env = call_env
        if "__file__" in func_def:
            self.current_file = func_def["__file__"]
        self.current_func = func_def.get("name", "<anonymous>")
        frame = {
            "caller": prev_func,
            "func": self.current_func,
            "file": prev_file or self.current_file,
            "line": self.current_line,
        }
        self.call_stack.append(frame)
        try:
            for stmt in func_def["body"]:
                self.execute(stmt)
            return None
        except ReturnException as r:
            return r.value
        finally:
            self.call_stack.pop()
            self.current_func = prev_func
            self.env = prev
            self.current_file = prev_file

    def eval_file_builtin(self, kind, node):
        import json
        args = [self.evaluate(a) for a in node["args"]]
        try:
            if kind == "Read":
                with open(args[0], "r", encoding="utf-8") as f:
                    return f.read()
            if kind == "Write":
                with open(args[0], "w", encoding="utf-8") as f:
                    f.write(display(args[1]))
                return args[1]
            if kind == "Append":
                with open(args[0], "a", encoding="utf-8") as f:
                    f.write(display(args[1]))
                return args[1]
            if kind == "Export":
                with open(args[0], "w", encoding="utf-8") as f:
                    json.dump(args[1], f)
                return args[1]
        except OSError as e:
            raise RuntimeError(f"{kind.lower()}x: {e}")
        raise RuntimeError(f"Unknown file op {kind}")

    def is_truthy(self, v):
        if v is None or v is False:
            return False
        if v == 0 or v == "" or v == []:
            return False
        return True