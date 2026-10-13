<!-- Copyright (C) 2026-2027 Ziad Mohsin. All Rights Reserved. -->
# Phase 3 design - the MountX interpreter in MountX

Status: **design, no code.** Every number in section 2 was measured today, on this
tree, before a line of `interp.mx` was written. Line citations are against
`interpreter.py` at 931 lines (`wc -l`; its last line has no trailing newline) and
`main.py` at 177 lines, both re-read for this document.

Two of the corrections are headline enough to name here - there is **no traceback
machinery and no `MX_TRACEBACK`** anywhere in this tree (6.4), and `printx` is
**a statement, not a native** (`interpreter.py:309`) - and section 15.1 lists all twelve
claims of mine that reading or measuring disproved, including two of my own counts from
the first draft of this file.

---

## 1. What phase 3 is, and the one wall it has to get around

**The goal.** `selfhost/interp.mx` is a MountX interpreter written in MountX: it
takes the AST `parser.mx` already emits byte-identically to `parser.py`, and it runs
it. Phase 2 proved the parser port against `parser.py`; phase 3 proves the
interpreter port against `interpreter.py`.

**The wall.** MountX runs at 57-73 AST nodes per second. Python runs the same
language at 675,000 node-visits per second. The gap is measured, not guessed:
**about 10,400x**. One extra interpretation layer multiplies the wall clock by that
factor, which is why phase 3's deliverable is a differential harness and a small
corpus, not a faster interpreter and not a bigger test suite.

Five constraints follow, and they shape every decision below:

1. **Layer 2 is the last real layer.** The anchor runs in about an hour at layer 2
   and in about 468 days at layer 3 (section 2.4). Nothing in this design pretends
   otherwise, and the bootstrap question is answered by what phase 3 *can* show
   (I-M6), not by what it cannot afford to run.
2. **The output budget is essentially free.** `fbtest.mx` prints 23 lines and 365
   bytes. The differential therefore compares *whole stdout*, no sampling, no
   digests (section 10).
3. **The recursion budget is a fixed 10,000 Python frames, shared** between the
   port's own ladder and every guest call it makes (section 8).
4. **104 behaviours must be transcribed, not reasoned about**: 16 statement kinds,
   17 expression kinds, 13 operators, 44 error raise-paths (42 distinct texts) whose
   wording a `catchx` can observe, 6 environment constructors, 8 `display` shapes
   (section 11.1). The 17 natives and their 29 argument guards are *free*, by delegation
   (9.3). This is
   why the differential is the deliverable and not the `.mx` file - phase 2's epilogue
   listed three defects found by diffing, and phase 3 has six times the surface.
5. **The host is not a library.** `interpreter.py` is 931 lines - five classes and one
   big tree-walking `Interpreter`;
   `interp.mx` cannot call it, so everything is ported, and the natives are the
   only place where the port is allowed to ask the host for help (section 9).

**Out of scope** (each revisited in section 14): closures, new natives, generator
bytecodes, JSON, `http_get`, REPL semantics, performance work, and any change to
`lexer.py`, `parser.py`, `interpreter.py` or `main.py` - **the port adds no host-file
change**, which is what makes the differential meaningful, since the oracle and the port
cannot be drifting in the same direction. Two preparatory edits were already made
outside `selfhost/` before this document (section 15.3): one rename that touched a
comment line in `interpreter.py`, and `fbtest.mx`'s scratch path.

---

## 2. The measurements first, and the anchor's runtime before any gate

Everything in this section is measured. Where something is derived the arithmetic is
shown, so the derivation can be checked against the measurements rather than
trusted.

### 2.1 The anchor, measured from the inside

`fbtest.mx` run on an instrumented copy of `interpreter.py` (counters only, no
behaviour change, `/tmp` copy, repository untouched):

| measured | value |
|---|---|
| entry AST nodes (after `main.py:30-33` auto-wrap) | **3,829** |
| modules the import machinery parses | **25**, **9,070** nodes |
| AST nodes total, entry + stdlib | **12,899** |
| statement lines | 415 |
| assertions | 277 |
| AST visits during execution | **249,168** (190,585 `evaluate` + 58,583 `execute`) |
| host function calls | 2,169 |
| deepest guest call nesting | **8** |
| deepest Python frame stack | 47 |
| stdout | **23 lines, 365 bytes** |
| `parse()` time | 0.005 s |
| execution time | **0.369 s** |

Two derived rates:

```
host parse rate          3,829 nodes / 0.005 s  =  765,800 nodes/s
host visit rate      249,168 visits / 0.369 s   =  675,251 visits/s   (1.48 us/visit)
```

`examples/tour.mx` in the same harness: 1,335,966 visits in 0.723 s, 100 stdout
lines. The tour visits 35,000 nodes per printed line; `fbtest.mx` visits 10,800
nodes per printed line. Neither is print-bound.

### 2.2 What one layer of MountX actually costs, measured on this tree

The phase-2 law is 57 nodes/s per layer, measured over the whole repo (22,227 nodes,
387 s). That figure includes 38 file switches and 22,227 dump lines. Measured
*alone*, on the anchor's own file, today:

```
selfhost/parse_repo.mx on one file (fbtest.mx): 3,829 nodes + 3,830 dump lines
    = 52.15 s   ->  73.4 nodes/s
```

The dump was byte-identical to the oracle's. The first diff showed one line,
`0a1 > === fbtest.mx ===`, which was my own inline oracle omitting the case header
that `parity_parse.py:271` adds - not a port defect. So the 52 s is a correct parse,
and 73.4 nodes/s is the same law as 57 with less harness cost. **Use 57-73 nodes/s
as the band.**

### 2.3 The two layer rates, and why they differ by 200x

```
parse layer:       3,829 nodes / 0.005 s in Python  =  765,800 nodes/s
interpreter layer:   249,168 visits / 0.369 s       =  675,251 visits/s
```

A Python parse node costs **1.31 microseconds**; an interpreter node-visit costs
**1.48 microseconds**. Same shape of work - one dict lookup, a dispatch, a recursion -
same cost. What differs by 200x is what the *port* spends per unit: `parser.mx`
spends 13.6 ms of MountX per node, because a node emission runs the schema loop,
`format_field`, and an output build. An interpreter visit does less work: a `Literal`
is one read, an `Ident` is a chain walk. So the port's interpreter layer should be
*faster per visit* than `parser.mx` is per node, not slower. Both estimates below use
the parser's rate as the visit rate anyway, which makes the first a bound rather than
a guess.

### 2.4 The projection, written before the first run

Two independent methods on the same anchor.

```
Method A - measured slowdown on a shared task
  765,800 / 73.4 = 10,433x      (same file, same work, both sides, measured today)
  Python cost of fbtest  = 0.005 + 0.369         = 0.374 s
  projected at layer 2   = 0.374 x 10,433          3,897 s = 65 min

Method B - equal-rate bound, the design number honoured
  249,168 visits / 73.4 nodes/s                     3,394 s = 57 min
  249,168 visits / 57.4 nodes/s                     4,341 s = 72 min

  optimistic bound  (method B, fast rate)           3,394 s   57 min
  planning number   (method A)                      3,897 s   65 min
  the 57-nodes/s law (method B, repo rate)          4,341 s   72 min
  if a visit costs 2x a node emission               7,794 s  130 min   <- red line
```

**The number to honour: `python3 main.py selfhost/interp.mx --run fbtest.mx` takes
about 65 minutes, expectation band 57-72 minutes, red line 130 minutes.** The
`2x` row is the failure mode to watch: 2.2 hours means a per-visit bug (env rebuilt
per node, defensive array copy, error-object allocation in a hot branch), not
physics.

Three things the projection settles before the run:

- **The parse is not the problem.** The port parses 12,899 nodes (entry + 25 stdlib
  modules) in 12,899 / 73.4 = **176 s = 2.9 min = 4.6%** of the run. The other 95%
  is interpretation. Speeding up the parse changes nothing.
- **Output is free.** 23 lines at 73 writes/s is 0.3 s out of 3,900. Whole-stdout
  comparison costs nothing (section 10).
- **Layer 3 is not a gate.** 3,897 s x 10,433 = 4.07e7 s = **468 machine-days** for
  one anchor run at layer 3. The scaling law is per *work item*, so a micro-anchor
  of 25 statements (~300 visits) is 300 / 73.4 x 10,433 = **12 hours** at layer 3,
  which is why section 13 spends exactly one milestone, I-M6, on layer-3 evidence
  and buys it with programs of tens of nodes, not hundreds.

### 2.5 Recursion, measured

| probe | deepest OK | first failure | calls at the cliff |
|---|---|---|---|
| `chain9` (phase 2's ladder, 9 functions) | D=260 | D=280 | 2,349 / 2,529 |
| `chain5` (5 functions, phase-3 shape) | D=480 | D=520 | 2,405 / 2,605 |

Both ladders die at **~2,400-2,530 MountX calls**, at a measured **4.00 Python
frames per call** through the host's tree-walker: 2,500 x 4 = 10,000 = exactly
`main.py:9`. Section 8 designs around this and corrects phase 2's wording.


---

## 3. Values

### 3.1 One array, three shapes, one tag

The host has eight value kinds and distinguishes them with Python `isinstance`
(`interpreter.py:91-111`). MountX has no `isinstance`, so every value the port
carries is an array whose first element is a kind tag:

```
[tag, ...payload]

  "N"   number        ["N", v]                     v is a host int or float, as-is
  "B"   bool          ["B", true|false]
  "Z"   null          ["Z"]
  "S"   string        ["S", "..."]
  "A"   array         ["A", id, arr]               arr: plain MountX array of boxes
  "F"   function      ["F", id, funcdefNode]
  "M"   bound method  ["M", id, methodNode, instanceBox, klassNode]
  "O"   instance      ["O", id, className, dict]   dict: name -> box
  "U"   module        ["U", id, modName, funcsDict, classesDict, letsDict]
  "P"   super         ["P", instanceBox, klassNode]
```

Three rules make this work, and each is here because the alternative breaks
observably.

**Every value is boxed, including numbers and booleans.** An unboxed number is
indistinguishable from an array length or a dict size in a language with no type
test, and the port's dispatch is `arr[0]`. The cost is one array read per value read;
the escape hatch is internal-only (section 9.4), never visible in output.

**Arrays are a stamp plus a plain MountX array - not a `count + dict`.** The host's
arrays are Python lists, and MountX arrays *are* Python lists, so wrapping one gives
the host's semantics with no work: index read and `arr[i] = v` are O(1) and in place
(`interpreter.py:418-427`), `+` copies (`:553-554`), `len` is the native, iteration by
index is O(1) per step, and **element order is guaranteed** - no dependence on
`dictKeys()` insertion order, which is the hazard a `count + dict` encoding brings.
There is also no quadratic-append problem to solve, because MountX has no in-place
push: `array.push(arr, v)` is `arr + [v]` (`stdlib/text/array.mx:202-204`), which
copies on both sides. And phase 2 already banked the cost of exactly this shape on
this tree - `parser.mx:742-744` builds every node list as `body = body + [x]`,
measured at 4,000 appends in 36 ms, with the largest list anywhere in the repo 152
entries - so `interp.mx` reads AST list fields with `len` and an index directly.
**`count + dict` is what `hashtable.mx` uses for maps, and this port needs dicts for
env frames, instance fields and the class registry - not for arrays.**

**Instances and arrays carry an identity stamp.** The host uses Python object
identity in two places: the last line of `values_equal` (`return a is b`,
`interpreter.py:547`) for instances, functions and modules, and `id(v)` in `display`'s
cycle guard (`:67-71`). MountX has no `id`, so the port mints a monotonic integer per
array, instance, function and module box. Aliasing copies the box, so it copies the
stamp; every operation the host treats as producing a new object (`+`,
`ArrayLiteral` at `:490-491`, `New`) mints a fresh one. That makes `a == b` on two
instances and `[...]` on a self-referencing array both *derived* rather than guessed.

**Instances are keyed by class *name*.** The host's instance holds a reference to the
class AST node and prints `v.klass['name']` (`interpreter.py:73-80`, `:105-106`).
Every consumer only needs the name: `type_name` returns it, `display` prints it, and
`resolve_class` looks classes up by name in one dict (`:439`). Inheritance walks
names, so the name is sufficient and the class object is not needed.

### 3.2 The four places `bool` is not a number

`is_num` is `isinstance(v, (int, float)) and not isinstance(v, bool)`
(`interpreter.py:51-52`). Because the port's numbers are host numbers, Python's
`bool is an int` footgun is inherited wholesale. There are exactly four guards in
the host and the port needs all four, in the same order:

1. `is_num` excludes bool - and the port's `is_num` is a *function*, because the
   host cannot inline the check across the box.
2. `values_equal` tests bool **first**, before null and before numbers
   (`interpreter.py:536-537`), so `1 == true` is false.
3. `loop` count rejects bool explicitly (`interpreter.py:328`).
4. `eval_index` rejects bool as an index (`interpreter.py:630`).

A fifth place appears in the port only: the box-tag comparison, where a `["B", ...]`
and an `["N", ...]` are different tags and so can never be confused. The tag makes
footgun 2 structurally impossible; footguns 1, 3, 4 are live and must be written.

### 3.3 `display` and `type_name` are the output contract

`display` (`interpreter.py:55-88`) is what `printx` prints, what string `+` produces
(`interpreter.py:551-552`), and what `writex` writes. Its rules, each of which the
port reproduces exactly:

| value | text |
|---|---|
| null / true / false | `null`, `true`, `false` - not Python's spellings |
| string | the string itself; with `quote_strings` (inside a container) `"` + s + `"`, **unescaped** |
| array | `[a, b]`, elements quoted, `", "` join |
| instance | `Name{k: v, ...}` in **field insertion order** |
| module | `<module name>` |
| FuncDef / BoundMethod | `<func name>` / `<method name>` |
| cyclic array / instance | `[...]` / `{...}`, via an identity-seen set |
| number | Python `str(v)` |

The last row is the one phase 2 banked and phase 3 inherits for free: the host's
numbers *are* Python numbers. **There is no `num_to_string` in this codebase** -
`display` falls through to `str(v)` at `interpreter.py:88`. The port therefore does
`"" + v`, whose measured behaviour is `"" + 7` -> `7` and `"" + 1.5` -> `1.5`,
identical to Python `str` for these values. Two spellings must be pinned by case
because they are easy to get wrong: `string.tonum("0.00001")` prints `1e-05` both
sides, and `idiv` returns an int while `/` returns a float, so `idiv(4,2)` prints
`2` and `4/2` prints `2.0`. Both are already asserted in `fbtest.mx`.

Cycles are detected by *identity* in the host (`id(v)`, `interpreter.py:67-71`), and
the port's stamp is what replaces it: `display` keeps a set of stamps on the current
descent, and prints `[...]` / `{...}` when a stamp reappears. Two aliases share a
stamp, so a self-referencing array is caught; a structurally identical copy is not,
which is also what the host does. `I5` is the case that proves it.

### 3.4 Reference semantics, which come free and which do not

The host's `MxInstance` is a Python object, so instances alias by reference and
`a = b` then `b.x = 1` mutates `a`. The port gets this from the dict: `["O", n, d]`
copied into another variable shares `d`, and `dictSet(d, "x", 1)` is visible through
both. Arrays likewise. **Nothing in the port may copy a dict defensively** - one
defensive `dictCopy` in the `Ident` read path would turn 277 passing checks into a
wall of failures, and cost about 2x per visit on top.

Where the host is *by-value*, the port must copy: array `+` (`interpreter.py:553-554`
returns a new list), `ArrayLiteral` (fresh each visit, `interpreter.py:490-491`), and
the `for...in` iteration snapshot (section 7.3).

---

## 4. Environments

### 4.1 The chain

The host's `Environment` is `{values dict, parent}` with three walkers
(`interpreter.py:114-147`). The port's is a two-element box:

```
env = [parentEnv, valuesDict]        null parent = the global env is a chain end
```

and the three walkers are three functions, each a `while e != nullx` loop over
`env[0]`:

| host | port | note |
|---|---|---|
| `define` (`:121`) | `envDefine` | `dictSet(env[1], name, v)` - always the current env |
| `get` (`:124`) | `envGet` | walk, and the miss is a *raised* `[Runtime] Undefined variable 'x'` |
| `set` (`:132-139`) | `envSet` | walk; **on total miss, define in the *current* env** |
| `exists_in_chain` (`:141`) | `envExists` | used only by `this` and `super` (`:480`, `:484`) |

**`envSet`'s fall-through is the whole scoping model.** The host's last line is
`self.values[name] = value` with the comment "assigning an undeclared name declares
it" (`interpreter.py:139`) - so an undeclared assignment lands in the *innermost*
env, not the global one. It is one line, it is invisible until it is missing, and
`fbtest.mx`'s `check_eq(T_COUNT, 3)` inside `T.check_eq` (`fbtest.mx:13`) only
prints `2` without it. **It is the first assertion I-M1 must satisfy**, and it is a
case in its own right (`I4`).

### 4.2 Where each env comes from

Six environments exist; five are one-line ports and the sixth is the module one.

| env | host | created by |
|---|---|---|
| global | `self.globals` (`:172`) | `Interpreter()` |
| block | `Environment(self.env)` (`:377`) | `execute_block`, for `if`/`while`/`loop`/`for` bodies |
| iteration | `Environment(self.env)` (`:391`) | `exec_forin`, one per iteration, holds the loop var |
| call | `Environment(parent)` (`:880`) | `call_function`, parent is `__mod_env__` or globals |
| catch | `Environment(self.env)` (`:367`) | `run_catch`, holds the caught value |
| module scope | `Environment(self.globals)` (`:243`) | `handle_import`, per imported module |

Two details that are load-bearing and easy to lose:

- **The iteration env is per-iteration, not per-loop** (`:391` is inside the
  `for idx, elem` body). A `for` whose body assigns to the loop variable must not
  leak to the next iteration; the port must build a fresh `[env, dict]` each step.
  The block env *inside* it is a second level, so a `for` body with a `let` is two
  envs deep per iteration. Both are host lines, both are cheap, and `I4` covers
  them.
- **`execute_block` pushes an env even for `if` branches** (`:377`), which is why a
  `let` inside an `if` is invisible outside it. The port's `execBlock` always pushes,
  even when the block is empty.

### 4.3 `__mod_env__`: the closure substitute, and why nothing more is needed

`handle_import` stamps `m["__mod_env__"] = mod_scope` on every imported func and
method (`:253-257`), and `call_function` reads it as the call env's parent, falling
back to globals (`:879`):

```
parent = func_def.get("__mod_env__") or self.globals
```

That single field is the entire lexical-scope story in this language: a function's
parent env is its module's top-level env, or globals. **A user-defined function has
no `__mod_env__` and therefore always sees globals** - which is why there are no
closures here and why phase 3 can claim closure-freedom honestly rather than by
hope. The port reproduces it exactly: the funcdef *node* is the function value
(boxed as `["F", node]`), the stamp is a `dictSet` on the node, and the read is a
`dictGet` with a null test. `I4` proves a module function sees a module `let`, and
proves a local function does *not* see its caller's locals.

The stamping must happen on the node, so **the port's AST is mutated by the
interpreter**, exactly as the host's is. The dump in section 13.1 still compares a
clean AST because the dump runs before interpretation; but this rules out comparing
an AST dump *after* a run, which is a trap phase 2 would have walked into.

---

## 5. Control flow: the six interactions, and the one channel design

### 5.1 What the host does, and what the port may not do

The host has four signals, all carried by Python exceptions (`interpreter.py:26-48`):

```
ReturnException(value)      :26      raised by Return          :345-347
BreakException              :31      raised by Break           :348-349
ContinueException           :35      raised by Continue        :350-351
ThrowValue(value)           :39      raised by Throw           :361-362
CONTROL_FLOW = (Return, Break, Continue)                        :48
```

and one rule that is the whole reason `tryx` is trustworthy: `TryCatch` re-raises
`CONTROL_FLOW` before it looks at anything else (`interpreter.py:355-356`,
"never swallow return/break/continue"), catches `ThrowValue` and passes the value
through unchanged (`:357-358`), and coerces every other Python exception to its
string (`:359-360`).

The port cannot use Python exceptions: it is MountX source, and its own `throwx` /
`tryx` would be the *language's* `throwx` / `tryx`, the very thing it is
interpreting. The question is what to use instead, and the answer has to satisfy six
interactions at once. The rest of this section enumerates them; the design that
satisfies all six is 5.8.

### 5.2 E1 - `throwx` caught in the same activation

`throwx e` (`:362`) evaluates `e` and raises `ThrowValue` with the value. `TryCatch`
(`:352`) runs the try body in a block env, and on `ThrowValue` calls
`run_catch(node, tv.value)` (`:366-374`), which pushes a fresh env binding
`node["catch_var"]` to the value and runs the catch body. **The value is never
stringified on this path** - the docstring at `:40-41` says so, and `fbtest.mx`
asserts it: `throwx "boom"` and `throwx t` where `t` is a `Thrower` instance both
come back with their types intact (`fbtest.mx:254-274`). Consequence for the port:
the throw channel must carry a **value box**, and the catcher must not touch it.

### 5.3 E2 - `throwx` unwinding across activations

A throw inside a guest function with no catcher in that activation must reach the
nearest catcher outside it. In the host this is free - the Python stack *is* the
unwind, and `call_function`'s `finally: self.env = prev` (`:901-902`) restores the
environment chain on the way out. In the port, the Python stack is still there (the
port's own `callFn` frames), but the port cannot stop on it: it has to *notice* on
the way back. So the unwind is a state read, once per level, and the design question
is where that state lives (5.7-5.8).

One consequence of "notice on the way back" is a correctness obligation the host
never has: the port must restore the interpreter's current-env pointer on the unwind
path too. The host does it with `finally`; the port has no `finally`, so **every
return path of every port function that pushes an env must restore it**, including
the unwinding ones. There are five such functions (`execBlock`, `callFn`,
`runCatch`, `execForIn`, `runModule`) and each has up to four exit paths. This is the
single most likely place for a phase-3 defect, and `I2` is built to catch exactly it:
a throw from three frames deep, caught at the top, followed by a `let` in the catcher
that must land in the *right* env.

### 5.4 E3 - the four signals must not be confusable

`TryCatch` distinguishes thrown values from control flow by *exception class*
(`:355` vs `:357`). The port has no classes, so the discriminator is data, and the
rule is: **a `tryx` in guest code sees `THROW` and nothing else.** `RETURN`,
`BREAK`, `CONTINUE` arriving at a guest catcher must pass through untouched - which
is the host's `except CONTROL_FLOW: raise`, reproduced as "the catcher checks the
kind and does not act on anything but `THROW`". Phase 2 hit precisely this shape and
called it `D1`; the case that keeps it dead is `I3`, whose guest program is a
`return` inside a `tryx` inside a `while` inside a function.

### 5.5 E4 - a host failure becomes a catchable value

`except Exception as e: self.run_catch(node, str(e) or e.__class__.__name__)`
(`:359-360`) means a division by zero is catchable as the string `"Division by
zero"`, so error text is **output the port owes**, not diagnostics. `interpreter.py` has
**73 `raise RuntimeError` sites**, and the AST walk that bucketed them by enclosing
function (`selfhost/measure_raises.py`, kept in the tree so the split is re-countable)
splits them three ways, not two:

| sites | where | the port |
|---|---|---|
| **29** | the native branches of `eval_call` (`:676-839`) | **free** - delegated onto the host's own message (9.3) |
| **6** | the call-dispatch tail of `eval_call` (`:846-872`) | **transcribed**: not-callable, bad `super(...)`, `Cannot call {t}` |
| **38** | everywhere else (`execute`, `evaluate`, `eval_binop`, `assign_target`, `eval_dot`, `eval_index`, `eval_new`, `class_chain`, `eval_file_builtin`, `Environment.get`, `handle_import`, `run`, `exec_forin`, `call_function`) | **transcribed** |

So **44 raise-paths, 42 distinct texts** (`Index {i} out of range` is shared by `:426`
and `:633`; `'{}' is not callable` by `:846` and `:853`), and `readx` / `writex` /
`appendx` / `exportx` interpolate a variable `str(e)` tail, so the count of *reachable
strings* is unbounded even though the raise sites are not. The port must not merely raise
on those paths, it must raise with the *same string*, and `catchx` must be able to see it.
That makes the message list part of the port, not a comment - which is why 14.1 makes
the 44-path sweep a corpus obligation rather than an afterthought.

The `or e.__class__.__name__` half is dead in practice (no MountX-raised error has an
empty message) and is ported as "the message is always non-empty", not as a fallback
branch.

### 5.6 E5 - a port bug must never become a guest value

If `interp.mx` has a defect - `dictGet` on a missing field, indexing a null, an
unknown node kind - the host would surface it as a Python `RuntimeError` and die
loudly. The port must not turn that into a guest catch value, because then a port bug
would be *indistinguishable from a guest error* in the one artifact that matters:
stdout would print `[Runtime] <something>` and exit 1, and the differential would
report a mere mismatch instead of a crash. So: **the port never uses `tryx`, and a
port-level failure surfaces as a Python-level crash with a traceback, which the
harness treats as a build failure, not a diff.** Section 10.4 makes that mechanical -
exit code 2 and a `PORT-CRASH` line, never a guest-shaped error.

### 5.7 The rejected alternative, and the reason it is rejected

The obvious design is to carry all four signals on the port's own `throwx`, tagged:

```
throwx ["PORTCF", kind, payload]
   and every catcher does
catchx(e)  if tag(e) == "PORTCF" then throwx e else handle(e)
```

It is less code - no kind slot read at ~20 call sites, no per-path env restore, and
the Python stack does the unwind for free. It is **rejected for soundness, not for
cost**: the tag is guest-observable data, so guest code can write

```
throwx ["PORTCF", "RET", nullx]
```

and the port's catcher re-throws it as a return from the enclosing guest function. A
guest value would be able to forge control flow. With a kind slot that only the port
writes, guest values are inert by construction. Phase 2's `D1` was a
channel-collision defect; this design would reintroduce a worse one.

Two secondary reasons, recorded because they are real but neither is decisive: the
tagged-throw design makes 5.6 impossible to state, since a port bug is then
*definitionally* a throw and must be distinguished by tag - the same forgeable
channel; and it puts a `throwx` in the hot path, which costs nothing when nothing
unwinds but makes a `let` statement's steady-state cost depend on the exception
machinery.

### 5.8 The design: one kind slot, six kinds, no `throwx` anywhere in the port

The port has **one mutable slot holding a frame** `[kind, payload, line]`, plus a
`[hasValue, value]` pair for returns. **Six** kinds, as integers:

| kind | value | set by | absorbed by | propagates through |
|---|---|---|---|---|
| - | 0 | reset on entry to every loop and function | - | - |
| `RET` | 1 | `Return` (`:345`) | `callFn` (`:899`) | loops |
| `BREAK` | 2 | `Break` (`:348`) | innermost `while` / `loop` / `for` | functions |
| `CONT` | 3 | `Continue` (`:350`) | the same three | functions |
| `THROW` | 4 | `Throw` (`:361`) | the enclosing `TryCatch` (`:357`) | everything |
| `ERROR` | 5 | every error path in 11.4 | the enclosing `TryCatch` (`:359`) | everything |
| `EOF` | 6 | `Input` at end of stdin (`:524`) | the enclosing `TryCatch`, as `ERROR` is | everything |

The first five are control flow. **The sixth is not, and it is there because of a
measurement rather than a plan.** `input()`'s end-of-file is the one error `main.py`
renders two different ways: caught, `:359-360` hands `catchx` `str(e)` and the guest
sees `EOF when reading a line`; uncaught, the same exception is not a `RuntimeError`,
so `format_error` falls through to its last arm (`:50-51`) and prints
`EOFError: EOF when reading a line`. One payload cannot be both strings, so the KIND
carries the distinction while the payload stays `:360`'s bare message - which is what a
catcher must receive. `format_uncaught` adds the class name, and `TryCatch` absorbs it
exactly as it absorbs `ERROR`, because a guest catching an EOF must get the message and
not a kind it cannot name. Case `g12` freezes both renderings; `I05` pins the split.

This is the same shape as the `THROW`/`ERROR` split immediately below, one level up:
where the host distinguishes two failures by Python's exception **class**, the port
distinguishes them by a kind integer, because the top-level renderer is the only place
the difference is visible. I-M4 added it as the sixth; a release that finds a seventh
such pair should expect the slot to still have room, and should re-read this paragraph
before assuming the payload can hold the difference.

`THROW` and `ERROR` are **one kind in the host and two in the port**, and the reason
is the top-level renderer. The host uses Python's exception *class* to tell them
apart, and `format_error` renders them differently (`main.py:42-49`):

```
ThrowValue      ->  [Runtime] Uncaught throwx: <display(value)>     main.py:42-43
RuntimeError    ->  [Runtime] <message>                             main.py:48-49
```

A `catchx` cannot see the difference - both go to `run_catch` (`:357-360`), which is
why one kind would be enough for the guest - but the *uncaught* case can, and
`fbtest.mx:254-259` proves the guest-visible half. Encoding the distinction as a
kind rather than a flag on the payload keeps rule 4 intact: the payload is always
just the value.

Four rules, and they are the whole of it:

1. **`exec*` / `eval*` never return a signal.** They write the slot and return a
   value; the value is meaningless when `kind != 0`. This keeps the hot path to one
   slot read where a branch is needed and zero reads where nothing can unwind
   (`Literal`, `Ident`, arithmetic).
2. **The slot is global to the interpreter, not per-activation**, because only one
   signal is ever in flight. `callFn` saves it on entry and restores it on the
   `RET`-absorbing path, exactly as `call_function`'s `finally` restores `self.env`.
3. **Absorbing clears it.** A loop that eats a `BREAK` sets `kind = 0`, as the host's
   `except BreakException: break` swallows the exception; `callFn` eats `RET` and
   clears. `THROW` is cleared only by a `TryCatch`. Forgetting a clear turns one
   `break` into a program-wide early exit - loud, and cheap to test.
4. **Guest values enter the channel as `payload` and only with `kind == THROW`.**
   This is the sentence that closes 5.7's hole.

Two properties follow, and they are the point of the design: the slot is written only by
port code, so **no guest program can forge control flow** (5.7); and the interpretation
path contains **zero `throwx`**, so a bug in `exec`/`eval`/`callFn` cannot be swallowed
by a guest `catchx` (5.6). The port does use `tryx`, in exactly one place - the native
call boundary (9.6) - where what it catches is a Python exception rather than a MountX
value, which is the one thing guest code cannot fabricate. That single exception is what
makes the design correct; a version without it is not faster, it is wrong (9.6 explains
the concrete program that breaks).

The cost of rule 1 is one array read per statement in blocks and per condition in loops.
Against a per-visit budget of 13.6 ms that is free, and it buys the two properties above.
**Decision I-D1.**

---

## 6. Errors, the two streams, and the exit codes

### 6.1 One formatter, one stream

`main.py` has a single error formatter (`main.py:36-50`) shared by the file, `-e`
and REPL paths, and `die()` writes it with a plain `print` (`main.py:55`) - **to
stdout, never stderr**, then `sys.exit(1)`. That is a gift to the differential: the
whole error surface is comparable content, and stdout is the only stream a correct
run produces. It also means the port must not "improve" anything here: error text is
output, and phase 3's rule is that error text is compared line for line.

The five renderings the port's driver must reproduce, verbatim:

| cause | text | citation |
|---|---|---|
| `SyntaxError` already labelled | message as-is (`[Lexer]` / `[Parser]` prefixes come from the port's own parser) | `main.py:38-41` |
| `SyntaxError` unlabelled | `[Syntax] <msg>` | `main.py:41` |
| uncaught `throwx` | `[Runtime] Uncaught throwx: <display(value)>` | `main.py:42-43` |
| top-level `return` | `[Runtime] 'return' used outside a function` | `main.py:44-45` |
| top-level `break` / `continue` | `[Runtime] 'break' or 'continue' used outside a loop` | `main.py:46-47` |
| `RuntimeError` | `[Runtime] <msg>` | `main.py:48-49` |
| anything else | `[Runtime] <ClassName>: <msg>` | `main.py:50` |

The middle four are the ones nobody tests and everybody breaks. They are also free
in the port: the driver inspects the kind slot after the guest's top level finishes
and renders - `RET` at top level is row 4, `BREAK`/`CONT` is row 5, `THROW` is row 3,
`ERROR` is row 6. **Cases `I8a`-`I8d`.**

`run_ast` catches `Exception` broadly (`main.py:62`), which is why a
`RecursionError` is reported as a runtime error rather than crashing, and the
measured string is `[Runtime] maximum recursion depth exceeded` - confirming
`RecursionError` reaches the `RuntimeError` branch, not the class-name branch. The
port's own depth limit (8.4) prints that same string, deliberately.

### 6.2 The two ways a run can fail, and how the harness tells them apart

| | guest-shaped failure | port crash |
|---|---|---|
| stdout | a `[Runtime]` / `[Syntax]` / `[Parser]` line, produced by the port | whatever the guest printed before the crash, and **no** bracketed line |
| stderr | empty | Python traceback |
| exit code | 1 (`main.py:56`) | 1 (Python's own) |
| what the harness reports | a diff, if the oracle disagrees | **`PORT-CRASH`, always a build failure, never a diff** |

Exit code alone cannot separate them - both are 1 - so **the classifier is
"non-empty stderr"**, which needs no change to any host file. Where the port does
detect an inconsistency in itself (a kind left uncleared, an env that will not
restore), it prints `PORT-BUG <detail>` to *stderr* and calls `sys_exit(2)`, so a
deliberate self-check is distinguishable from both. **Case I9** asserts the
classifier by feeding the harness a deliberately crashing port.

### 6.3 `sys_exit`, and the argument list the guest sees

`sys_exit` calls Python's `sys.exit`, which raises `SystemExit`; `die` passes it
through (`main.py:54`), so a guest `sys_exit(3)` exits 3 with no message
(`interpreter.py:703-707`). Under the port the native is the same call, so the
behaviour is identical by delegation.

The argument list is worth spelling out because the two runs are not the same
command. The host: `python3 main.py prog.mx a b` gives `sys_args()` = `["a","b"]`
(`interpreter.py:701` returns `argv[2:]`). The port: `python3 main.py
selfhost/interp.mx prog.mx a b` gives the *port* `["prog.mx","a","b"]`, and the port
must hand the guest the tail - which is exactly one shift. So `sys_args` is delegated
with the port consuming its own `argv[0]` and passing the rest, and `fbtest`-style
programs see the same list both ways. **Case I10.**

**Corrected by I-M2, and the correction is a design change, not a footnote.** A
*constant* shift cannot be right, because the port is invoked as
`main.py run_probe.mx <port_dir> <guest> a b` and `sys_args()` inside the port is the
host's native reading the *host's* `sys.argv` - the tail the port would compute by
inspection hands the guest its own filename. There is also no way to fake the host's
argv from inside a MountX program: `sys_args` is `interpreter.py:697-701` reading
`self.argv`, and the port has no handle on that list. So 6.3's "delegated with a shift"
is not implementable as written, and what shipped instead is a register: `interp.mx`
holds `g_guest_args` (a raw host list of strings, set by the exported `set_guest_args`),
`sys_args` answers from it through the reboxer, and the *driver* - the only party that
knows where the guest's own path sits - builds the tail from index 2 and installs it.
The port's `sys_args` is therefore the one native in the table that is **not** a
delegated call, and its guard text (`sys_args expects 0 arguments`, `:698-699`) loses
9.3's free pass and is transcribed. What the differential proves today is narrower than
the section's original claim, and the difference is worth keeping in view: the harness
runs both sides with **no** guest arguments (`parity_interp.py:266`, `:268`), so
`g6_calls.mx`'s `printx(sys_args())` freezes `[]` on both sides. That row still has
teeth, because the naive reading prints the port's own path - `["selfhost",
"exec_cases/g6_calls.mx"]` - and a golden that says `[]` is a claim that the answer came
from the register rather than from the host's argv. The version with arguments on the
command line, which is the one `I10` was written for, needs the harness to pass them and
is I-M5's, where the natives that take paths force the same plumbing anyway.

### 6.4 Correction: the traceback machinery does not exist

Earlier in this phase I described a `self.traceback` list appended by
`call_function`, an `MX_TRACEBACK` environment switch, and a renderer at
`main.py:69-79`, and proposed a differential gate that would turn it on. **None of
that is in this tree.** `grep` for `traceback` and `MX_TRACEBACK` across all 85
files returns nothing; `call_function` (`:874-902`) has no traceback code, and
`main.py:69-79` is `run_file`'s read-and-compile block. The line numbers were real,
the content was not. Dropping the gate.

It matters for more than tidiness: a gate on dead code would have passed forever,
which is phase 2's `V1` shape exactly. The lesson is the one already in this
document's first paragraph - **cite by reading, not by recollection**, and section
15.1 lists this and the `printx`-as-native error alongside the fix.

---

## 7. Statements, expressions, and the loops MountX does not have

### 7.1 Two dispatchers, 33 kinds, ported in order

`execute` dispatches 16 statement kinds in this order (`:304-362`):

```
Let  Assign  Print  ExprStmt  If  While  Loop  ForIn  FuncDef  ClassDef
Import  Return  Break  Continue  TryCatch  Throw            + else: "Unknown stmt"
```

`evaluate` dispatches 17 expression kinds (`:475-530`):

```
Literal  Ident  This  Super  ArrayLiteral  BinOp  UnaryOp  Call  DotAccess
Index  Input  TypeOf  New  Read  Write  Append  Export      + else: "Unknown expr type"
```

The port mirrors both chains **in the host's order**, as `if`-chains on the node's
`kind` string - the shape phase 2 measured at 12-17 microseconds per lookup against
4.5 ms for a hashtable get (PARSER_DESIGN 11). Over 249,168 visits the whole dispatch
costs under 4 s of a 3,900 s run, **0.1%**. Order is kept for auditability and not for
speed: a reviewer must be able to read `exec()` against `execute()` line by line and
see the same program. **Decision I-D2: transcription first; no dispatch
chain is reordered to buy 0.1%.** The one shape change worth naming: `Read`/`Write`/`Append`/
`Export` are one `in (...)` test in the host (`:529`) and four `if`s in the port,
because MountX has no `in` over a tuple - the same deviation phase 2 recorded as
`D-2`.

Node access is `field(node, key)` / `hasField(node, key)`, with `hasField` reserved for
the places the host itself asks whether a key exists: `node["else"]` (falsy-vs-missing,
`:316`), `klass.get("init")` (`:589`), and `__mod_env__` (`:879`). Everywhere else the
oracle indexes directly and so may raise `KeyError`, which reaches `run_catch` as a
name-only string (`:360`) - a shape no valid program reaches, and case `I11` pins the
port's behaviour as *crash*, not silence.

### 7.2 The loop translation, which is the only genuinely new code in phase 3

The host's loops are Python `while` / `for range` / `for enumerate`, and each absorbs
`BreakException` / `ContinueException` in a `try` inside the loop body
(`:318-336`, `:399-406`). The port's loops are MountX loops, which have no `try`
inside them and no `do-while`. The translation is one idiom, applied three times.

**Block walk.** The host's `execute_block` stops on the first exception and does not
absorb anything. MountX's `while` re-tests its condition after every iteration, so the
port writes the walk as a guarded index loop and the guard *is* the stop:

```
execBlock(stmts):  push env
                   let i = 0
                   while i < len(stmts) && kind == 0 { exec(stmts[i]); i = i + 1 }
                   pop env            // on every path: 5.3
```

**`while`.** The host order is: test condition, run body, absorb. The port must not
test the condition when a signal is pending - a `break` in the body must leave the
loop without evaluating `arr[i]` once more, because that read could itself raise and
change the error. So the pending-kind test comes first, and the `continue` case clears
and falls through to the condition, which is exactly the host's `except
ContinueException: continue`:

```
execWhile(node):   while kind == 0 && truthy(eval(node.condition)) {
                       execBlock(node.body)
                       if kind == CONT { kind = 0 }          // absorbed: re-test
                   }
                   if kind == BREAK { kind = 0 }             // absorbed here
```

Three consequences worth stating, because each is a case: `break` and `continue` are
*absorbed* here and nowhere above (`:322-325`); an `ERROR`/`THROW` pending at the top
of a loop exits without touching the condition, and the trailing `if kind == BREAK`
does not clear it, so it keeps unwinding; and a `RETURN` propagates through the loop
untouched, which the host does by never catching `ReturnException` in the loop at all.

**`loop N`.** Two host-only details: the count must be an int and not a bool, or
`loop count must be an integer` (`:328-329`), and the range is `max(0, count)`
(`:330`), so a negative count runs zero times. In the port, the host's int-ness is not
queryable - `isinstance(x, int)` has no MountX equivalent, and phase 2's port already
relies on that gap (`mx_int` is exact only below 2^53). But the port does not need
`isinstance`: `display` distinguishes `3` from `3.0` and from `3.5`, so the check
becomes *"the rendered text contains no `.` and no `e`"*, which accepts `3`, rejects
`3.0` and `1e+20`, and matches the host's int/float split for every value the language
can produce. **This is one of two places in the port where the host's Python type test
has to be re-expressed, and it is also why `display` is written first** (3.3). The
loop body then runs `min(count, available)` times through a MountX `loop`, and case
`I12` covers `loop -1`, `loop 0`, `loop 2.0`, `loop true`.

**`for x in`.** The host iterates `enumerate(iterable)` over the *evaluated-once*
iterable (`:387-390`), so a body that reassigns the iterable variable still walks the
original array, and a body that appends (`a = a + [x]`, which copies) cannot extend the
walk. The port walks an index over the box it captured at entry, which reproduces both
by construction. Strings are iterable and yield 1-character strings (`:388` admits
`str`); non-arrays raise `for...in expects an array or string` (`:389`). Two-variable
`for i, v in` defines the index first and the element second (`:392-394`), and the
per-iteration env (4.2) is pushed *inside* the walk. **Case I13.**

### 7.3 Operators, equality, truthiness

Eleven binary operators plus `AND`/`OR`, and the order of the guards is the
language's own invariant, not an implementation detail (AGENTS.md, "breaking any of
these recreates a fixed bug"):

1. `AND`/`OR` short-circuit **in `evaluate`**, not in `eval_binop` (`:495-502`), and
   `OR`'s right side is reached only when the left is falsy. The port must not
   evaluate a right side it does not need: `false && undefinedVariable` is not an
   error.
2. In `eval_binop`, `PLUS` is tested first and **string wins over number** - either
   side being a string makes it concatenation via `display` on both (`:551-552`), so
   `"a" + 1` is `"a1"` and `1 + "a"` is `"1a"`. Array-plus-array is next
   (`:553-554`), then numeric (`:555-556`), then `Cannot add <t> and <t>` (`:557`).
3. `EQEQ`/`NEQ` go to `values_equal` on *any* types (`:558-561`), which is where
   3.2's bool rule lives.
4. `GT`/`LT`/`GTE`/`LTE` accept number-number **or** string-string and reject anything
   else with `Cannot compare <t> and <t>` (`:562-569`) - before the numeric guard,
   which is what makes `"b" > "a"` work.
5. Only then does `needs numbers, got <t> and <t>` apply (`:570-571`), then
   `MINUS`/`MUL`/`DIV`/`MOD`, with `Division by zero` (`:578`) and `Modulo by zero`
   (`:582`) checked **before** the operation, so `0/0` is the message and not `nan`.

`/` always yields a float (`:579`) and `%` keeps Python's sign rule (`:583`) - both
free, because the port computes with host numbers. `is_truthy` (`:927-932`) is falsy
for `null`, `false`, `0`, `""`, `[]` - and note the host's `v == 0` also catches
`0.0`, while `false == 0` is `True` in Python, so **`if false { }` is falsy by the
explicit `v is False` test on `:928`, not by the `== 0` on `:930`**. Ported in the same
order with the same two tests: bool first.

### 7.4 Evaluation order, which is observable through errors

Five places where the host's evaluation order changes which error a program reports,
and the port must not reorder them: `BinOp` evaluates `left` before the operator
dispatch and `right` after (`:494`, `:503`); `Call` evaluates all arguments left to
right before dispatch (`:514`), so `f(g(), h())` reports `g`'s error first; `Index`
evaluates the array then the index (`:626-627`); `Assign` evaluates the value, then
walks the target (`:306-308`, and for an index target the array then the index,
`:419-420`); and `New` resolves the class before evaluating arguments (`:636-640`).
Case `I14` runs five programs that each trigger the *later* side's error and asserts
which message the host prints. Getting one of these backwards is invisible in a
green run and unfixable later, which is exactly why it gets a case rather than a
paragraph of reasoning.

### 7.5 Call dispatch, and the one order that must not change

Arguments are evaluated by `evaluate` (`:514`); `eval_call` then dispatches on the
**callee node's shape**, not on the value (`:672-872`): `Ident` (natives first, then
`env.get`, `:674-846`), `DotAccess` (bound method or module function, `:847-853`),
`Call` (a call returning something callable, `:854-861`), `Super` (nearest `init` up
the chain, `:862-871`), else `Cannot call <t>`.

The natives test comes **before** the environment lookup (`:676` precedes `:841`), and
AGENTS.md lists that as an invariant: a user function named `len` or `idiv` must not
shadow the native. The port's `evalCall` checks its native chain in the same position
and for the same reason, and case `I15` asserts it with `func len(x) { return 99 }`.

`callFn` (`:874-902`) is short and every line of it is load-bearing: arity is checked
against `params` with the host's exact message (`:875-877`); the parent env is
`__mod_env__` or globals (4.3); `this` is defined before params (`:881-882`); `super`
is defined only when the *defining class* has a parent, and its `klass` is the
**parent**, not the defining class (`:883-890`) - AGENTS.md calls this out as the
reason multi-level chains resolve, and `I7` is the three-level case; params are
zipped in order (`:891`); and the `finally` restores `self.env`, which in the port
becomes four restore sites (5.3).

---

## 8. The recursion budget, which is shared and fixed

### 8.1 What was measured, to the frame

`main.py:9` sets `sys.setrecursionlimit(10000)`. Two ladders, bisected to a step of
one, with the instrumented host reporting the Python frame stack at the deepest point:

| probe | shape | deepest OK | first `RecursionError` | calls in flight | Python frames |
|---|---|---|---|---|---|
| `chain5` | 5 functions per guest level | D=498 | D=499 | **2,490 / 2,495** | - |
| `chain9` | 9 functions per guest level (phase 2's) | D=276 | D=277 | **2,484 / 2,493** | - |
| `chain5` at D=498 | instrumented | - | - | 2,495 | **9,983** |

Both ladders die at the same **call count**, not the same nesting depth, and the
instrumented run gives the reason directly: **`py_frames_per_call` = 4.00**, so 2,495
calls x 4 = 9,983 frames, within 17 frames of the 10,000 limit. The two ladders agree
because their per-call frame cost is the same; the ladder's width is irrelevant.

**So the currency is Python frames, the budget is 10,000, and the host's tree-walker
spends 4.00 of them per nested MountX call.** Phase 2's section 11 recorded the same
cliff as "2,250 MountX frames surviving, 2,475 dying" and called the unit *frames*; the
unit was calls, and the missing constant was 4.00. Correcting the label changes no
phase-2 conclusion - the cliff it measured is the cliff measured again here, at 2,484
rather than 2,475 because its ladder was 9 wide and bisected coarser. **This is the
number phase 3 designs around.**

### 8.2 The same 10,000 frames now have to cover two interpreters

At layer 2 every guest call is executed by the port's own ladder, and each *port* level
is itself a host-interpreted MountX call, so it costs the same measured 4.00 frames. The
port's cycle per guest call level is five port functions deep - `callFn` -> `execBlock`
-> `exec` -> `evalExpr` -> `evalCall` - so:

```
frames per guest level, layer 2   =  5 port levels x 4.00  =  20
guest levels available            =  (10000 - ~40 driver) / 20  ~=  498
frames per guest level, layer 3   =  20 x 4.00  =  80          (25 port levels)
guest levels available, layer 3   ~=  120
```

The 498 was derived, not measured, and **I-M2 measured it and the model lost**. The
instrument is `selfhost/depth_probe.mx` (`python3 main.py selfhost/depth_probe.mx
<depth> <shape>`): it builds the guest ladder as text, lifts the port's own counter out
of the way, and lets the interpreter's real stack run out. Bisected to a step of one on
this machine, with the port's `interp.mx` parsed and run by the host:

| ladder shape (one guest call per level) | deepest OK | first `RecursionError` | expressions nested per call |
|---|---|---|---|
| `return 1 + L0(k - 1)` | 275 | 276 | 1 |
| a ring of 5 functions per level | 66 levels (330 calls) | 67 (335) | 1 |
| nine nested expressions (probe `x`) | 115 | 120 | 9 |
| nineteen nested (probe `y`) | 66 | 67 | 19 |
| thirty-nine nested (probe `z`) | 36 | 37 | 39 |
| seventy-nine nested | under 20 | not bisected | 79 |
| the plain ladder through `run_probe.mx` instead of the probe | 275 | 300 | driver-dependent by ~25 levels |

Four things are different from the model, and all four matter:

- **The port's ladder is seven deep, not five.** The chain a call sits on is `callFn →
  execList → exec → evalExpr → evalExpr/evalArgs → evalCall → callFn`. 8.2 counted the
  five it had written in 7.2 and missed the two that `return 1 + f(k - 1)` walks through.
- **The ceiling is a property of the guest's shape, and it falls FASTER than the nesting
  grows.** Nesting goes 1 → 9 → 19 → 39 → 79 and the ceiling goes 275 → 115 → 66 → 36 →
  under 20: doubling the nesting from nine to nineteen does not halve 115, it lands on
  66. Each extra `evalExpr` frame is a port frame and each of those is ~4 host frames, so
  the cliff tends toward zero as a body gets heavy. **No constant is universally below
  it**, which is the fact 8.3's choice has to be made against.
- **The "implied frames per level" column this table used to carry was circular.** It
  divided an assumed 10,000-frame budget by the measured ceiling and reported the
  quotient as a rate; the usable stack is nearer 1,800 frames on the cheapest ladder and
  the rate is not constant across shapes. The columns that survive are the two measured
  ones. Nothing downstream read the third column, which is why the error was survivable.
- **498 was never reachable, 400 was on the wrong side of the cliff, and so was 100.** At
  the derived number the counter is decoration: the physical stack runs out first, in the
  middle of the port's own frames, skipping the four restore sites of 5.3 - the exact
  failure 8.3 was written to prevent. 100, the first retake, was the same defect in
  miniature: the nineteen-nested shape cliffs at 66, *under* 100. It took finding that
  the probe's widest generator did not parse at all - 18 openers against 19 closers in a
  typed literal, so the shape the design called "what pushes the cliff lowest" had never
  been run once - for the real number to appear. The generator is built by a loop now
  (`nest(n)`), and the tail above is measured rather than inferred.

**Bisect on the exit code and nothing else.** A run that fits exits 0, a physical
overflow exits 1 with `main.py`'s line, and a guest that fails to *parse* exits 2 - which
reads as "fits" to anything watching stdout. That is the whole story of how width `y`
hid for a milestone.

Three things this settles, restated against the measured numbers:

- **The anchor is nowhere near the cliff.** `fbtest.mx` reaches guest depth 8: about 290
  host frames at layer 2 on the cheapest ladder, 2.9% of the nominal 10,000 recursion
  limit and nearer **16% of the ~1,800 frames the port can actually use** - the honest
  budget from the bullet above. Still nowhere near the cliff, and it never nests an
  expression deeply enough to pay a heavy body's rate. Recursion is not a reason the
  anchor can fail; it is a reason some *other* program will.
- **The budget is shared with the parse ladder, and the two can stack.** `parser.mx`
  gives out near 250 levels of nested parens - a ladder of roughly 4 port levels per
  construct, about 16 frames each, 4,000 frames. Parsing normally happens at guest depth
  1, so it never stacks with a guest ladder. **Except when the guest parses at depth**:
  an `importx` inside a recursive function, or a program that parses text inside a loop
  body, puts a 4,000-frame parse ladder on top of a guest ladder. The host tolerates
  that; the port does not. Divergence `D-3`, and the corpus rule is that no case
  `importx`es below depth 4. The measured guest ceilings (330 calls on a ring, 66 on a
  body that nests nineteen expressions) make this sharper than it looked at 498: a guest
  ladder of 60 levels plus a parse ladder is already past most of the budget.
- **Layer 3's ceiling is a few guest levels, not ~120.** Each guest level at layer 3
  sits on the layer-2 port's ladder as well as the layer-3 one, so it pays roughly the
  layer-2 bill again: on the measurements above, a plain body that reaches 275 at layer 2
  reaches somewhere near 60-70 at layer 3, and a body that nests nineteen expressions
  reaches single digits. That estimate is arithmetic on measured cliffs rather than a
  measured cliff - phase 4 measures it - but the layer-3 demo's program has to be much
  shallower than 13.3 assumed either way, and its *statement count* is capped at tens by
  2.4, which is now the same order of constraint rather than three orders above it.

### 8.3 The port's own counter, and why it makes the divergence smaller

`interp.mx` keeps a MountX-level depth counter incremented in `callFn` and raises
`ERROR "maximum recursion depth exceeded"` at a declared limit. That string is chosen,
not copied: `RecursionError` is a `RuntimeError` subclass, so the host's reaches
`main.py:48-49` rather than the class-name branch and prints exactly
`[Runtime] maximum recursion depth exceeded` - verified against the real failing output
of both ladders above, and again against the port's own refusal in `g8_recursion.mx`'s
neighbourhood.

**Set the limit at 40.** It was 400, then 100, and both were inert in the same way: a
counter above the physical cliff never fires, and when the Python `RecursionError` is
what arrives instead, it unwinds the port's frames past the four restore sites of 5.3
with `g_depth` left high (the decrement is a statement, and a Python exception does not
execute statements) and - the part that is not merely untidy - catchable by a guest
`tryx`, which lets a program carry on running with a counter that no longer describes the
stack. A counter that only fires when nothing else would has no job.

Since no constant is below every cliff (8.2), the limit is chosen by a rule rather than
by a margin: **sit below the cliff of every shape that could still have finished
something, and accept the escape for shapes whose own ceiling is already under the
limit.** At 40 the escape set is the shapes that nest ~35 expressions or more per call,
and their ceiling is 36 or less - a run one of them gets past 36 is a run no limit can
give them. Lower the limit further and the trade inverts: at 36 the `z` shape is caught,
and a `y` guest at depth 40 - which the port *would* have completed, its cliff is 66 -
is refused instead. 40 is where the refusals stop costing runnable programs.

| guest depth | host | port | differential |
|---|---|---|---|
| <= 40 | runs | runs | matches |
| 41 - the shape's cliff (66 to 330 calls on the shapes measured) | runs | `[Runtime] maximum recursion depth exceeded`, exit 1 | **divergence `D-4`** |
| a shape heavier than ~35 nested expressions, 37-40 | runs | the Python `RecursionError` escaping the port - the residual the rule accepts, on a shape that dies by 36 regardless | both lines are the same bytes |
| past the host's own cliff (~2,490 calls in flight) | `[Runtime] maximum recursion depth exceeded`, exit 1 | the same message, from the counter or from the physical stack | **matches in text** |

The shape of the argument is unchanged and the constant is not: the divergence is *where
the cliff is*, never *what the cliff says*, and the corpus stays under the limit by
construction - `g8_recursion.mx` now runs its deepest ladder at 24 and its mutual
recursion at 30 frames, because with the limit at 40 the old 40-deep ladder and
`even_p(41)` would have been the refusal, and a refusal in a golden is `D-4` frozen. What
changed is the width: the band is the gap between 40 and whatever the guest's shape can
reach, and the port's ceiling is a tenth of the host's on a plain body and a fortieth on
a heavy one. **Decision I-D3, amended twice by measurement: `MAX_GUEST_DEPTH = 40`, with
the comment citing 8.1's rate, 8.2's table and this one.** The counter is one
increment and one compare per `callFn`; against 13.6 ms per visit, free.

### 8.4 What must not be done about it

Raising `setrecursionlimit` for the port run changes `main.py`, which phase 3 does not
touch, and buys back a ladder whose depth is worthless at 10,400x cost - a program using
2,000 levels would take weeks at layer 2 regardless. Setting the port's limit to a
measured cliff is worse, and 8.2's measurement is the reason to say so precisely: the
cliff is a function of the guest's expression nesting (275 plain, 115 at nine nested
expressions, 66 at nineteen, 36 at thirty-nine) *and* of the driver's own frame usage
(275 through `depth_probe.mx`, ~300 through `run_probe.mx` on a plain body), so an exact
match is a coin flip that fails ninety minutes into the anchor run. Both declined.

### 8.5 Alternatives on recursion, with their costs

| option | what it buys | what it costs | verdict |
|---|---|---|---|
| counter at 40, shared budget, message-matching | the counter is what fires for every shape whose ceiling is above 40 - every shape that could have finished something; five times fbtest's deepest recursion | the 41-to-cliff band (`D-4`), plus the escape for shapes heavier than ~35 nested expressions, which die by 36 anyway; one compare per call | **take it** |
| counter at 100 | the same, on cheap shapes | inert on the nineteen-nested shape, whose cliff (66) is below it, so the Python error escapes exactly as it did at 400 | **declined by measurement (the I-M2 review)** |
| counter at the derived 400 | (as written before I-M2) a narrower-looking band | inert: below it the physical stack dies inside the port, past the restore sites of 5.3 and catchable by a guest `tryx` | **declined by measurement** |
| counter at 36 or lower, to catch the heaviest shapes too | the `z` shape's escape closed | refuses `y`-shaped guests between 37 and 66 that the port would otherwise complete - a refusal that costs a runnable program | declined |
| raise `setrecursionlimit` in `main.py` | ~2.5x depth | a host-file change phase 3 disclaims; C-stack risk | declined |
| set the port's limit to a measured cliff | 15-230 more usable levels | a cliff that moves with the guest's shape and the driver's stack, so it can fire mid-run | declined |
| trampoline `evalExpr`/`exec` into one loop | 2-3x guest depth at layer 2, because the held ladder is what the ceiling divides into | abandons transcription and side-by-side auditability; ~2-3x per-visit cost | declined for phase 3; the lever if a later phase needs depth |
| `threading.stack_size` + worker thread | effectively unbounded depth | a host-file change; thread-local state in the driver | declined |

### 8.3a I-M4's re-measurement: the class ladder, and why 40 still holds

Classes introduce a guest shape the earlier measurements did not contain: **one guest
level can cost three activations** - the call, the `init` call that `newx` makes, and
the method call. `depth_probe.mx` grew a `c` width for exactly this, a ladder whose body
is `let box = newx Node(k)` then `return box.get() + L0(k - 1)`, measured the same way
as the other five (counter lifted, caller bisects on the exit code):

| shape | activations per guest level | counter boundary (guest levels) | physical cliff (guest levels) |
|---|---|---|---|
| plain ladder (8.2) | 1 | 40 | 275 |
| **class ladder (I-M4)** | **3** | **39** | **~275** |

The counter's boundary barely moves - 39 against 40 - while the cliff stays put, so
**the counter still wins by ~7x** and `MAX_GUEST_DEPTH = 40` remains a rule rather than
a margin. That is the whole reason the constant is expressed in CALLS: a shape that
triples its activations per level triples its cost against the counter, and the margin
absorbs it. The corpus follows g8's rule and keeps a class ladder at **24** deep
(`g13`), because a refusal inside a golden would freeze `D-4` as if it were agreement.

### 8.3b I-M5's re-measurement: the loop ladder, and the margin that narrowed

Loops are the last shape, and they answer the opposite question. A loop does not recurse
by itself, so the question is not "how many activations per level" but "does the loop
scaffolding make each level cheaper or dearer". The `l` width is a recursive call
wrapped in a `for` body, so each level also pays a block env (`:377`) and a
per-iteration env (`:391`):

| shape | activations per guest level | counter boundary | physical cliff | margin |
|---|---|---|---|---|
| plain ladder | 1 | 40 | 275 | ~7x |
| class ladder (I-M4) | 3 | 39 | ~275 | ~7x |
| **loop ladder (I-M5)** | **2** | **40** | **~157** | **~4x** |

**The cliff is what moved, not the counter.** The loop ladder's physical ceiling is
~157 against the plain ladder's 275, because each level holds a block env and an
iteration env *and* the port's own frames at the same time - so the interpreter's
stack, not the guest's call count, is the binding constraint. The margin therefore
narrows from ~7x to **~4x**, which is the first time any shape has come near making the
constant look like a margin rather than a rule. It still is one: 40 is below every cliff
here, and the two shapes that would narrow it further (a loop inside a class inside a
loop) were not measured, because 8.3's argument is about *each shape individually* and
a composed shape is a new measurement rather than a new limit. **`g17` keeps a loop
ladder at 24** for g8's reason, and if a future shape narrows the margin below 2x the
honest move is to re-open `I-D3`, not to lower 40 for the new shape alone - a limit
tuned per shape is a limit that has stopped being a limit.

---

## 9. The natives, and who is allowed to run them

### 9.1 The census, measured off the source

The host touches the outside world in 24 places. Seventeen of them are named natives,
matched by name inside `eval_call` **before** the environment lookup (`:674-846`):

| native | line | effect | native | line | effect |
|---|---|---|---|---|---|
| `len` | `:676` | none | `math_floor` | `:741` | none |
| `time_now_ms` | `:684` | clock | `idiv` | `:747` | none |
| `sleep_ms` | `:690` | clock | `db_open` | `:757` | file, sqlite |
| `sys_args` | `:697` | argv | `db_close` | `:770` | sqlite |
| `sys_exit` | `:703` | process | `db_exec` | `:783` | sqlite |
| `os_env` | `:709` | environ | `db_query` | `:805` | sqlite |
| `os_cwd` | `:715` | cwd | `http_get` | `:831` | network |
| `os_listdir` | `:721` | filesystem | | | |
| `os_sep` | `:730` | none | | | |
| `mx_version` | `:736` | none | | | |

Seven are effect-free. The other seven named natives, plus four that are node kinds
rather than names - `input` (`:520-524`), and `readx` / `writex` / `appendx` /
`exportx` through `eval_file_builtin` (`:529-530`, `:904-925`) - plus `printx`
(`:309-310`, a *statement*) and the import machinery's own file read (`:229-235`).

Two things follow immediately. **The port needs no new capability at all**: it reads
the guest's source with `readx`, which is a language feature, and it imports with
`readx` plus `parser.mx`, so the phase-1 and phase-2 rule that no new natives may be
added survives phase 3 intact. And **delegation is nearly free**: a native is one host
call inside a 13.6 ms visit, so the choice between delegating and reimplementing is not
a performance question at all. **It is only a correctness question**, which is what
makes it decidable.

### 9.2 The three policies

**Option 1 - port the arithmetic, delegate the world.** Re-implement the seven
effect-free natives (`len`, `math_floor`, `idiv`, `mx_version`, `os_sep`, plus the
arity/type guards of the rest) in MountX and delegate only the seventeen with effects.
It is the choice that *looks* most like self-hosting. It loses on three specifics:
`idiv` is `abs(a) // abs(b)` with a sign fix (`:752-755`), and MountX has no `//`, so a
ported `idiv` must go through `mx_int(a / b)` - which is wrong above 2^53, exactly the
gap PARSER_DESIGN records as open, and wrong for `idiv`'s int-ness (7.2) because the
host's `//` on two ints returns an int and `mx_int` cannot reproduce that distinction
for float inputs. `math_floor` needs `mx_floor`, which is itself a native, so
"porting" it adds a MountX round-trip over the same call. And `len` is O(1) as a native
and O(n) as a MountX walk that has to handle strings and arrays differently (`:679-681`).
**Three new bug classes for zero measured gain.**

**Option 2 - delegate everything, ask no questions.** Every native stays a native call
and the port does not implement one. Identical *by construction*: the guest's `time`
and the oracle's `time` are the same Python function, so there is nothing to drift. The
weakness is not the port, it is the *harness*: `fbtest`-shaped programs call
`time_now_ms`, `os_cwd`, `os_listdir`, so a corpus run compares wall-clock-dependent and
machine-dependent text and becomes flaky. Option 2 without a determinism policy is a
gate that fails on Thursdays.

**Option 3 - delegate everything, and put the policy in the harness. Recommended.**
Same delegation table as Option 2, plus three harness rules that make the comparison
meaningful: a normalizer applied to *both* sides for the values that cannot match
(`time_now_ms`, `sleep_ms`'s duration, `os_cwd`, `os_env`, `os_listdir` order,
`sys_args`); an explicit allow-list of env vars a corpus program may read, so
`os_env("HOME")` cannot leak the operator's machine into a "pass"; and a per-case
`nondeterministic:` header that says which normalizer applies, so the flakiness is
declared rather than discovered. The port stays a faithful mirror - **its correctness
is never conditional on the policy** - and the policy only decides what may be compared.

Option 3 also keeps the layer-3 story honest: delegated natives work at any depth,
because the innermost port's native call is still the same Python function, so nothing
in the policy has to be re-thought for I-M6. **Decision I-D4.**

### 9.3 The delegation adapter, which is the actual work

Delegating is not calling the native with the port's arguments: the port's values are
boxes, and the host's natives check types with `isinstance` (`:679`, `:691`, `:711`,
`:723`, `:748`, `:758`, `:784`). Handing `len` a `["N", 5]` would raise `len expects a
string or array, got array` where the host raises `Division by zero`-class errors about
something else entirely. So every delegated call is three steps:

```
1. unbox the arguments       2. call the native        3. rebox the result
```

Step 1 is shallow for scalars and one array level for `len` (`:679` only asks whether
the argument is a list or a string, and never looks inside), and **deep** for the two
places the host walks the value: `db_exec`/`db_query` params (`:797-799`, `:819-821`)
and `exportx` (`:921`). Step 3 is deep in one place too: `db_query` returns rows as
lists of scalars (`:822-827`) and each element must come back boxed.

A useful consequence: **the arity and type guards do not have to be ported at all.**
`Function 'f' expects N args, got M` is the port's own check (`:875-877`), but
`len expects 1 argument` (`:678`) and `os_env expects 1 string argument` (`:711`) are
raised by the host's native, on unboxed arguments, so the messages are identical by
construction. That is 17 native guards off the transcription list in section 11, and it
is the concrete reason Option 1 is not just pointless but *negative*: reimplementing a
native means reimplementing its guard message.

**Corrected by I-M2, which is where calls arrived and the claim was tested.** The
sentence is true about the *host's* guards and unusable as a plan, because it assumes the
port can hand the guest's argument list to the native, and **MountX has no splat**. The
port calls `native_dispatch(name, a)` with a fixed number of written arguments, so a
zero-argument `len()` cannot reach `:678` to be refused there - the port dies indexing
`a[0]` and 9.6's catcher turns the port's own defect into a plausible guest error reading
`list index out of range`. Six of the seventeen guards are pure arity (`len expects 1
argument`, `time_now_ms expects 0 arguments`, `sys_args expects 0 arguments`, ...) and
most of the rest are written as one combined test in the host (`len(args) != 1 or not
is_num(args[0])`), so the arity half and the type half cannot be separated at the call
site either. What shipped: `native_guard` transcribes all seventeen in the host's words,
in the host's order, *before* dispatch - which is 17 texts back onto the transcription
list in 11.1, taking that count from 29 free to 12 (the stateful refusals: sqlite,
`OSError`, `idiv: division by zero`, `db_exec: unknown connection 3`, and the type name
`len` renders for a non-string, which stays delegated because only the host can name a
type). The lesson generalises to the rest of the adapter and is worth one line: *a guard
is free only if the bad call can reach the guard*, and with no splat, arity guards
cannot.

### 9.4 `exportx` is the case that decides it

`exportx` is `json.dump(args[1], f)` on the **raw value** (`:919-921`). If the port
passed its own boxes, the file would contain `["N", 5]` instead of `5`, and the
differential on the written file would fail while stdout matched - a divergence
invisible to the primary gate. Worse, three of `json.dump`'s failure modes are
observable through `catchx`: a `MxInstance` is not serialisable, a cycle raises
`Circular reference detected`, and int-versus-float changes the bytes (`5` vs `5.0`).

Under delegation the port deep-unboxes and hands `json.dump` exactly the value graph the
host would have handed it, so the bytes, the `TypeError` text, the cycle error and the
`5` / `5.0` distinction are all **the host's own**, produced by the host's own json
module. Reimplementing that is the single largest trap in the native list - it means
porting Python's `json` encoder, including its float repr - and delegation makes it
free. **`I16` writes the file both ways and compares bytes**, because stdout cannot see
this class of bug; `I17` does the same for `writex`/`appendx` (which go through
`display`, so they are *not* the same case) and for a `db_query` round-trip.

### 9.5 The one place the port may bypass its own boxes

Boxing is a port-internal representation, so the port may use an unboxed value
*internally* wherever no guest program can observe it: the env chain's dict keys are
plain strings, the class registry's values are node arrays, the kind slot holds an
integer. What may never be unboxed is a value that crosses into guest view - a return
value, an array element, an env binding, a `display` argument - because that is exactly
where `typeofx`, `values_equal` and `display` need the tag. This is the discipline
phase 2 called "the escape hatch is internal-only", and it is worth restating because
in phase 3 an unboxed value leaking into an env binding would not crash: it would
change a `typeofx` result and print a different string.

### 9.6 The one catcher the design cannot do without

Found by trying to break the design, and it breaks the naive version with one line of
guest code. A native's failure is a **Python exception raised inside the host while the
host is executing the port**, so in the naive port it sails straight past the port's own
dispatch and out of the process:

```
tryx { readx("/no/such/file") } catchx(e) { printx(e) }
```

The oracle catches this: `eval_file_builtin` turns the `OSError` into
`RuntimeError("readx: ...")` (`:923-924`) and `TryCatch`'s `except Exception` hands the
string to `catchx` (`:359-360`), so it prints `readx: [Errno 2] No such file or
directory: '/no/such/file'` and exits 0. The naive port calls `readx`, Python raises
inside the *port's* frame, the harness sees a traceback on stderr, and the case is filed
as `PORT-CRASH` when it is in fact a passing test. **The same applies to every native
guard message** (`len expects 1 argument`, `:678`), which 9.3 counted as free - it is
free only if the raise is *catchable*, and a Python exception is not catchable by a
kind-slot design.

The fix is a catcher at the boundary, and it is sound for a reason that does not depend
on discipline: **a guest value cannot become a Python exception.** The channel into
`ERROR` from the interpreter's own logic is the kind slot (5.8); Python exceptions reach
the port only from the host's natives. So `callNative` is:

```
callNative(name, boxedArgs):
    let plain = unbox(name, boxedArgs)        // outside the catcher, deliberately
    tryx { let r = dispatch(name, plain); return rebox(name, r) }
    catchx(e) { setError(str_of(e)); return nullx }        // host error -> ERROR kind
```

Two details carry the weight. **Unboxing happens outside the `tryx`**, so a defect in the
port's own unboxer stays a crash (5.6) instead of being laundered into a plausible guest
error - the catcher's blast radius is one host call and its rebox, not the port's
argument handling. And **the payload is the exception's string**, because that is exactly
what `:360` gives the guest, so `catchx` sees `readx: [Errno 2] ...` and not
`OSError: readx: ...`.

**I-M7's boundary rule, and the limit this policy implies.** Delegation answers the
HOST's context, which is right for every native that touches the machine and WRONG for
one that asks a question about the *run*: `sys_args` is the program's own argv, and the
host's argv is not the port's. Two layers mask this because only one layer asks; three
do not, and the inner interpreter then boots the wrong file. **So a delegated native
answers the host's context, and a self-hosting layer must inject its own** - the rule,
its one exception in `eval_call`, and the regression case are all in 13.2.0a. The
generalisation for future self-hosting depths: any native whose answer depends on which
program is running, rather than on the machine, cannot be delegated across a layer
boundary and has to be injected at it.

This also explains why the port cannot simply let Python exceptions propagate as its own
control channel: they are the *host's* failures, and merging them with the guest's would
recreate 5.7's hole with worse consequences. **Decision I-D6: one `tryx`, in
`callNative`, with unboxing outside it; case `I20` runs the `readx` program above and
asserts exit 0 with the message printed.**

---

## 10. What the differential compares, and what it must not

### 10.1 Whole stdout, byte for byte, plus the exit code

The primary gate is `diff <(python3 main.py G) <(python3 main.py interp.mx G)` on the
complete stdout, plus the exit code, for every program `G` in the corpus. No digests,
no line counts, no summaries - those exist to save an output budget the phase-3 anchor
does not spend. Measured: **`fbtest.mx` emits 23 lines and 365 bytes; the tour emits
100 lines.** Against a 3,900 s run, comparing every byte costs 0.01% and losing a bug
because a digest said "same" costs the phase.

Two additions to stdout, both measured rather than assumed:

- **Written files**, for `writex` / `appendx` / `exportx` / `db_open` cases (9.4). The
  harness runs each in a fresh `$TMPDIR` directory and diffs the files too. This is why
  `fbtest.mx`'s scratch path moved to `$TMPDIR` before phase 3 started (15.4).
- **The elapsed time**, recorded as a first-class output of every run: `elapsed_s`,
  `peak_rss`, and the frame high-water mark. The projection in 2.4 is a *prediction*,
  and I-M2 exists to replace it; recording the number every run is what stops the
  replacement from being folklore.

### 10.2 The 277 is the oracle for the anchor, not the gate

`fbtest.mx` prints `OK - 277 check(s) passed` and nothing else on success. Under the
port it must print **that same line, and the same 22 lines above it**. The 277 is
therefore a *differential* signal, not a pass criterion: the port printing `OK - 261
check(s) passed` would be a *silent* failure if the harness only grepped for `OK`. The
harness diffs text and never greps. **Case I18** is the negative control: a port
deliberately broken to skip one check must make the gate red, and the harness's own
test suite asserts that it does. Phase 2's `P-M6` built exactly this control for the
parse gate, and phase 3 inherits the pattern.

### 10.3 Non-determinism, measured on the anchor itself

The anchor is the test, so the first question is whether it is deterministic. Measured:
`fbtest.mx` run twice, byte-identical, exit 0 both times. Then the reason why, which is
better than the policy I first wrote for it - **the anchor contains no clock, cwd,
argv, or directory-listing call at all** (`time_now_ms`, `os_cwd`, `os_listdir`,
`sys_args`: zero occurrences). The two environment- and state-dependent things it does
are both already reproducible by construction:

```
fbtest.mx:460-463   random.seed_rng(42) / rand_below / seed_rng(42) / rand_below
                    -> asserts the two draws are equal: determinism is what the
                       case is testing, so it cannot be a source of flake
fbtest.mx:49        os.env_or("TMPDIR", "/tmp")   -> a path, never printed
fbtest.mx:421       os.env_or("MX_DEFINITELY_NOT_SET_12345", "fallback")
                    -> an env var chosen to be unset, so it is a constant
```

So the anchor needs **no normaliser whatsoever**, and the policy below is written for
programs that are not the anchor. That is a stronger position than "we normalise our
flakiness away", and it came from measuring instead of assuming - the first draft of
this section claimed the anchor called `time_now_ms` twice and needed a clock
normaliser. It does not, and a normaliser built on that claim would have been a place
to hide a real divergence.

| source | in the anchor? | policy for corpus programs |
|---|---|---|
| `time_now_ms`, `sleep_ms` | **no** | **banned from the corpus.** There is no version of a timestamp comparison that is worth an hour of rerunning; a case that needs a clock is a case for the native's own unit test, not for the differential. |
| `os_cwd`, `os_listdir` | **no** | **banned.** `os_listdir` returns the repository's contents, which are not part of a test's meaning, and `os_cwd` makes a case depend on how the harness was invoked. |
| `os_env` | only for `TMPDIR` and one var chosen to be unset | **those two patterns only**: `TMPDIR` for scratch, and a name that cannot exist. Reading `HOME` or `PATH` is banned - it leaks the operator's machine into a "pass". |
| `sys_args` | **no** | allowed only with a fixed argument vector, and it is the vehicle for case `I10`'s one-argument-shift check (6.3). |
| `stdlib/core/random.mx` | yes, **seeded with a literal 42** | allowed **only** with a literal seed, the pattern `fbtest.mx:460` already sets. A case that cannot say its seed is not written. |
| `db_*` | yes | allowed: connection ids are a monotonic counter from 1 (`:763-766`) and sqlite is deterministic for a fixed statement set; the `.db` file lives in `$TMPDIR`. |
| `http_get` | no | **banned** - not for determinism, but because a green gate that depends on a network is not a gate. |
| `mx_version` | no | allowed, and must not be stubbed: a port that reports the wrong version is a port that does not know what language it interprets. |

The rule underneath the table: **normalise nothing that the corpus does not force.**
Each permission above is granted per source, here, and not by whoever is holding the
harness at review time. The design ends up with **zero** normalisers, because every
source that would need one is banned instead.

### 10.4 Classifying a red run before reading it

A differential that takes an hour per case is only usable if a failure says what kind it
is. The harness's first three checks, in order, cost nothing:

1. **stderr non-empty → `PORT-CRASH`.** The port's own defect (5.6, 6.2); the Python
   traceback is the artifact, and the case is not compared at all.
2. **exit 1 with an empty stderr → guest-shaped.** Both sides end this way for real
   errors, so the diff itself is the evidence; a `[Runtime]` line on one side and not
   the other is a divergence in the error path, which is almost always a message-string
   or an evaluation-order bug (7.4).
3. **exit 0, stdout differs → semantic.** The interesting category, and the only one
   where the line diff is worth reading.

A timeout is its own category and is not a crash: the harness's budget per case is the
2.4 band x 1.5 (so the anchor gets 97 minutes), and a timeout reports the *fraction* of
the expected output produced. A run that emits 22 of 23 lines and then stops for 90
minutes is not a wrong interpreter, it is a `while` loop the port cannot exit, and
reading its diff would be a waste of an hour.

---

## 11. The transcription surface, counted

### 11.1 What has to be ported, and what does not

| surface | count | where | note |
|---|---|---|---|
| statement kinds | **16** | `:304-362` | 7.1 |
| expression kinds | **17** | `:475-530` | 7.1 |
| binary operators | **11** | `:549-584` | `AND`/`OR` live in `evaluate`, not here |
| unary operators | **2** | `:504-512` | |
| error raise-paths to reproduce | **44** (42 texts) | 73 `RuntimeError` sites: 38 outside `eval_call` + 6 call-dispatch inside it | 5.5, 11.3 |
| environment constructors | **6** | 4.2 | |
| `display` value shapes | **8** | 3.3 | |
| value tags | **10** | 3.1 | *not added to the total*: five are the box kinds, three are already the `display` shapes above, and `void` / `class` are not values in the port (3.1, 4.1) |
| **total behaviours to transcribe** | **104** | | |
| native argument guards | **29, free** | `:676-839` | delegated onto the host's own messages (9.3) - but the other 6 sites in `eval_call` are call dispatch, not natives, and are counted above |

Arithmetic is not on this list, because the port's numbers *are* the host's numbers:
`/` returning float, `%` keeping Python's sign, and the int/float distinction that
decides whether `4/2` prints `2.0` while `idiv(4,2)` prints `2` are correct by
construction. Only the *guards* around arithmetic are ported. That is the largest single
saving in the phase, and it is why Option 1 in 9.2 is not merely pointless but negative:
reimplementing arithmetic converts a free correctness into a transcribed one.

**44 error paths to transcribe, 29 native guards free** is the fact that decides the
delegation policy, and it was counted off the AST rather than estimated - the first
draft of this document said 25, the second said 38 and called all 35 sites inside
`eval_call` free, which forgot that `eval_call` also dispatches calls.

### 11.2 What the anchor covers, measured off its own AST

The 277 assertions look like total coverage of the language, because `fbtest.mx` is the
test suite. Measured against the 33 dispatch kinds, using the anchor's AST dump and the
repo-wide dump phase 2 produced:

| | kinds hit of 33 | misses |
|---|---|---|
| `fbtest.mx`'s own source | **24** | `Continue` `Loop` `This` `Super` `Read` `Write` `Append` `Export` `Input` |
| anchor + every stdlib source it imports | **29** | `Continue` `Super` `Export` `Input` |
| whole repository corpus, node counts | `This` 799, `Loop` 5, **`Super` 2, `Continue` 1, `Export` 1, `Input` 0** | |

Source absence is not execution absence - `This` is absent from the anchor's text and
present 799 times in code the anchor imports and runs - but the converse holds: a kind
with no nodes anywhere near the anchor cannot be executed by it. **`Continue`, `Super`,
`Export` and `Input` are dispatch kinds the anchor, the stdlib, and nearly the whole
repository do not exercise.** Two `super` nodes in 22,227 is not coverage of a feature
whose `klass` binding AGENTS.md flags as a historic bug source, and `continue` appears
once in the entire corpus.

Phase 2's epilogue made this point qualitatively - coverage claims come from counting,
not from how big the corpus looks. This is the counting, and it sizes phase 3's corpus:
**the anchor certifies 29 kinds; the differential corpus exists for the four it does not,
interpreter message.

### 11.3 The 42 messages, grouped by the symptom a wrong transcription produces

Grouped by how a failure would be diagnosed rather than by line order:

| group | messages | cites |
|---|---|---|
| **names** | `Undefined variable '{n}'` · `Bad assign target {t}` · `'this' can only be used inside a method or init` · `'super' can only be used inside a method of a class with a parent` · `'super' used but this class has no parent` | `:130` `:429` `:481` `:485` `:488` |
| **call** | `Function '{n}' expects {} args, got {}` · `'{}' is not callable` (twice: ident `:846`, field `:853`) · `Call result is not callable` · `Cannot call {t}` | `:876` `:846` `:853` `:861` `:872` |
| **super-call** | `'super(...)' can only be used inside a method of a class with a parent` · `No init found in super classes for super(...)` | `:866` `:871` |
| **operators** | `Cannot add {t} and {t}` · `Cannot compare {t} and {t}` · `'{op}' needs numbers, got {t} and {t}` · `Division by zero` · `Modulo by zero` · `Unary '-' needs a number` · `Unknown binop {op}` · `Unknown unary {op}` | `:557` `:565` `:571` `:578` `:582` `:508` `:584` `:512` |
| **indexing** | `Cannot index a {t}` · `Index must be an integer` · `Index {i} out of range (length {n})` · `Array index must be an integer` · `Index assignment target is not an array` · `Cannot assign to field of non-instance` | `:629` `:631` `:633` `:424` `:422` `:417` |
| **classes** | `Class '{c}' not defined` · `Class '{c}' has no init but ...` · `Class '{c}' extends unknown class '{p}'` · `Inheritance cycle involving class '{c}'` · `super has no method '{f}'` | `:648` `:668` `:467` `:459` `:622` |
| **members** | `Module '{m}' has no attribute '{f}'` · `Module '{m}' has no class '{c}'` · `Field/method '{f}' not found on {c}` · `Cannot access field '{f}' on {t}` | `:606` `:643` `:615` `:623` |
| **loops** | `loop count must be an integer` · `for...in expects an array or string` | `:329` `:389` |
| **imports, io** | `Module '{n}' not found. Searched: [...]` · `{kind}x: {e}` · `Unknown file op {k}` | `:225` `:924` `:925` |
| **internal** | `Expected Program node` · `Unknown stmt {t}` · `Unknown expr type: {t}` | `:288` `:364` `:531` |

Four are harder than they look:

- **`Module '{n}' not found. Searched: {search}`** (`:225`) embeds the whole search-path
  list, assembled from `$MOUNTX_STD`, `script_dir`, the cwd and the interpreter's own
  directory, de-duplicated in order (`:196-218`). Byte-exact reproduction means
  reproducing that list, in order. **The port's list names the port's directory, not the
  interpreter's**, so the message differs whenever an import genuinely fails.
  **Divergence `D-1`**, accepted and documented: the corpus contains no program that
  imports a missing module, and the difference is message-only, never semantic.
- **`Inheritance cycle involving class '{c}'`** (`:459`) is detected with `id(cur)`, the
  same identity question as 3.1. `class_chain` walks *names*, and two classes in
  `self.classes` cannot share a name, so a name-set and an id-set agree here. Recorded as
  a deliberate substitution rather than an oversight; case `I6` pins it with
  `A extends B`, `B extends A` and a three-cycle.
- **`Index {i} out of range (length {n})`** appears **twice**, at `:426` (assign) and
  `:633` (read), with identical text - which is why the sweep is 44 paths but 42 texts.
  `'{}' is not callable` is the other pair, `:846` (identifier) and `:853` (field), and
  the two are *not* interchangeable at the call site: one names a variable, one names a
  field. One helper in the port is fine, provided both strings match. Trivial, and
  exactly the kind of thing a 44-row checklist exists for.
- **`'{}' is not callable` at `:846`** fires after the natives list and `env.get` have
  both succeeded, when the value found is neither a `FuncDef` nor a `BoundMethod` - so
  `let f = 3; f()` must reach this string and not the `Undefined variable` one. The order
  of 7.1's `Ident` branch is therefore part of the message, not just of the semantics.

### 11.4 The port must fail loud where the host fails loud

Every one of the 44 is on a path where the host raises. The port's corresponding path
sets `ERROR` with the same string. The failure mode to design against is the port
*silently* producing a null instead of raising, because a null flows onward, prints
something plausible, and turns a one-line diff into a forty-minute hunt through a
65-minute run. Two mechanical guards:

1. **No default in a required read.** `field(node, key)` on a missing key sets `ERROR`
   naming the key; it never returns null. Required fields are read with `field`, optional
   ones only after `hasField`, mirroring exactly where the host itself reaches for `.get`
   - `node.get("var2")` (`:392`), `node.get("prompt")` (`:522`), `klass.get("init")`
   (`:589`), `klass.get("methods", [])` (`:591`), `func_def.get("__mod_env__")` (`:879`) -
   and indexes with `[...]` everywhere else, including `node["else"]` (`:316`), which is
   a *falsy* test on a key that is always present.
2. **Unknown kind is fatal.** `exec` / `evalExpr` end in an `else` that sets `ERROR`
   with the host's own `Unknown stmt` / `Unknown expr type` text (`:364`, `:531`) -
   never a silent return. A dropped dispatch case therefore reports itself by name
   instead of quietly evaluating to null, which is how a 33-case chain stays auditable
   after it has been written.

---

## 12. Imports, classes, and the two identity traps

### 12.0 The sweep closure, measured rather than counted

11.3 claims "the 42 messages", which is 44 raise-paths because two texts appear twice.
That arithmetic sat in a document for two milestones without anyone recomputing it, so
I-M5 made it a tool: **`selfhost/reach_audit.py`** reads every `raise RuntimeError` out
of `interpreter.py`, asks whether the frozen goldens contain that text, and reports the
difference. `--check` fails if the numbers drift from what this milestone recorded, so a
new message in `interpreter.py` shows up as a failure rather than as a number somebody
has to remember to update. It also has a negative control: perturbing the expected count
makes it exit 1.

**The measured answer: 71 distinct texts across 73 raise-paths in the whole file, 68
frozen, and exactly 3 unreachable.** The 71 is larger than 11.3's 42 because that count
covered the deterministic core; the rest are the native guards and the `db_*` family,
which 9.3 said delegation makes free - free meaning the host's own words arrive through
the boundary, not meaning they were never worth freezing. I-M5 froze 25 of them in three
programs (`s45` seventeen stateless guards, `s46` the stateful `db_*` refusals, and the
two arity guards on the stateful natives), because a port that reworded any of them
would be more consistent and less correct, and that is precisely the failure a
differential exists to catch.

The three that no program reaches, each with the reason rather than an excuse:

| line | text | why no program reaches it |
|---|---|---|
| `:488` | `'super' used but this class has no parent` | `:883-890` binds a super box only when the DEFINING class has a parent, so the env never holds a null super; a parentless method hits `:485` instead. Pinned by an `I05` forged row. |
| `:678` | `len expects 1 argument` | the lexer gives `len` its own token, so the parser rejects `len()` with an RPAREN error and the bare form never reaches `eval_call`. It is a **parser** message wearing a runtime line number - `I15`'s row. |
| `:839` | `http_get failed: {e}` | only a live network call reaches it, and 10.3 **bans** `http_get` from the corpus. The arity half (`:833`) *is* frozen, in `s45`, with no network at all. |

So the honest total is **68 frozen + 3 unreachable = 71, with zero untested**. The
design's earlier "44 paths, 42 texts" was never wrong about the core; it was just
counting a subset, and it now says so.

### 12.0a The one class of message this audit structurally cannot see

I-M6 prep found a fourth category, and it is a limit of the tool rather than a gap in
the corpus: **a message the host does not raise itself.** `exportx` hands its value to
`json.dump` (`:922`), and json raises `TypeError: Object of type MxInstance is not JSON
serializable`. `interpreter.py` has no `raise` for it, so `reach_audit.py` - which
reads `raise RuntimeError` out of that file - cannot see it at all.

That is worth stating precisely because the port got it **wrong** while the audit was
green. `unbox_val` refused unserialisable values with a hand-written
`milestone 2 does not unbox ... for natives` - a leftover that was not a `too_far`
caller, and therefore survived I-M4's sweep of that function's callers. The guest saw
the port's sentence where the oracle prints json's. Fixed in I-M6 prep, with
`unserializable_text` reproducing the host's wording.

The type name is not always the box's own tag, and the measured reason is worth keeping:
**json walks a container before it reports.** A `BoundMethod` is a dict holding
`instance`, `klass` and `method` (`:613-614`), so json descends into it and the first
value it cannot encode is the *instance* - `exportx(p, obj.m)` reports `MxInstance`, not
`dict`. A port answering from the tag alone prints a line the oracle never prints.

So the audit's real claim is narrower than "no message is untested": it is **no message
that `interpreter.py` raises is untested**. Messages from a layer below - json, sqlite,
the OS - are covered by cases rather than by the audit, and `g19` is what covers these.
The honest fix is not to teach the audit about every library underneath, which is
unbounded, but to say out loud which boundary it polices.

### 12.1 `handle_import`, which is 97 lines of search order

The import machinery (`:185-281`) is the largest single unit phase 3 ports, and almost
all of it is order. Four bases - `$MOUNTX_STD` if set, `script_dir`, the cwd, this
file's directory (`:196-203`) - each expanded over three roots (`stdlib`, `libs`, `""`)
and seven sub-folders (`""`, `core`, `data`, `structs`, `text`, `math`, `system`) in
that nesting order (`:205-210`), de-duplicated keeping first occurrence (`:212-218`),
first existing file wins (`:219-223`).

The order is not cosmetic: it decides **which file is found**, and the 25 modules
`fbtest.mx` pulls in live in five different sub-folders. The port reproduces the
cartesian product in the same order with the same de-duplication, using `readx`'s own
success-or-null as the existence test, since MountX has no `os.path.exists`. Two notes:

- **`script_dir` is the entry script's directory** (`main.py:75`), so the port's driver
  must set it to the directory of the *guest* path, not to `selfhost/` - that is what
  makes `importx` work from any cwd (AGENTS.md states it as a requirement).
- **A missing module is `D-1`** (11.3): the message's path list does not match, and - the
  half this line first missed - neither does which modules *exist*, because base 4 is the
  oracle's `interpreter.py` directory and the port's `selfhost/`. With no `$MOUNTX_STD` a
  `stdlib/` module is found by the oracle and not by the port, so the exec corpus imports
  only its own `libs/` (13.2.3, `D-1`).

The rest of the function is four rules, each of which has a case:

| rule | cite | case |
|---|---|---|
| an import runs the module's top level **once**, guarded by an `_imported` set | `:186-188` | `I19a` - importing twice must not double-run its prints |
| module top-level `let`s land in the module's own scope, **not** globals | `:243`, `:273-276` | `I19b` |
| a module function must not shadow a module object of the same name | `:258-260` | `I19c` - this is the fixed bug AGENTS.md warns about |
| a transitive import's funcs and classes are **re-exported** with `setdefault`, so the first definition wins | `:262-272` | `I19d` - this is why `collections.mx` works |

And the module object is only defined when it has something in it (`:280-281`), so a
module of only `importx` lines defines nothing.

### 12.2 Classes: four functions and one allocation trap

`resolve_class` checks the global registry first, then a dotted `module.Class` (`:434-449`) -
so a local class shadows a stdlib class of the same name, which is load-bearing for a
program that defines its own `Stack`. `class_chain` walks parents, raising on an unknown
parent and on a cycle (`:451-468`, 11.3). `find_method` looks in one class only and
**checks `init` explicitly**, because `parse_class` stores it outside `methods` (`:586-594`)
- miss that line and `super.init()` answers `Field/method 'init' not found`.

`eval_new` (`:636-670`) resolves the class - dotted `module.Class` or bare name
(`:638-648`) - evaluates the arguments, validates the chain, then builds fields by
walking the chain **in reverse, parent first, so a child's field of the same name wins**
(`:653-656`). Field initialisers are `self.evaluate`d inside that loop (`:656`), so a
field whose initialiser is `[1,2]` gives each instance its own array. **Case I7c** builds
three instances and mutates one field of one.

The init is not the class's own but the **nearest init up the chain** (`:660-666`), and
`call_function` is handed `init_class` as the defining class - so `super` inside an
inherited `init` binds to the *inheriting* class's parent chain, not the definer's. A
class with no init anywhere in its chain still accepts **zero** arguments and raises
`Class 'X' has no init but N argument(s) were given` if handed any (`:667-669`).

**The trap is that `BoundMethod` is allocated fresh on every field access**
(`:613-614`, and again at `:620-621` for `super`). It follows that in the host:

```
obj.m == obj.m     ->  false      # values_equal falls through to `a is b` (:547)
```

The port reproduces this only if it mints a **new stamp for `M` on every `DotAccess`**.
Caching the box - which is the natural optimisation, and the one that would save real
time at layer 3 - would make that comparison `true`, i.e. the port would become more
consistent than the language. **Decision I-D5: no bound-method cache; stamps per access;
case `I7b` asserts `obj.m == obj.m` prints `false`.** Recorded as a decision because it
costs performance to be faithful, and someone will eventually want to "fix" it.

### 12.2a I-M4 as shipped: three things the reading did not say

**`eval_new`'s dotted arm and `resolve_class`'s dotted arm do not agree, and that is
load-bearing.** `resolve_class` catches a missing module and returns `None`
(`:443-446`), so `extends mod.Missing` reports `extends unknown class`; `eval_new`
calls `self.globals.get` directly (`:641`), so `newx mod.Missing` reports
`Undefined variable 'mod'`. Unifying them would be a silent behaviour change, so the
port transcribes both (`eval_new` uses `envExists` + the `Undefined variable` text,
`resolve_class` keeps its `try`). One case each would freeze the difference, and the
dotted-`:643` half is `s37`.

**A module body's classes land in the GLOBAL registry, so an import can overwrite a
local class.** `g_klass` declares `classx Square extends Shape`, and running that
module's top level registers *its* `Shape` into `self.classes` - so a program that
declares its own `Shape` **after** the import gets the module's one, and `newx Shape()`
then calls the module's `init` and reports an arity error. Both sides reproduce this,
so it is the host's behaviour and not a port defect, but it is a trap: the case
declares the local class **first**, and says why in a comment, because a reader who
reorders those two lines gets a confusing arity error rather than a shadowing result.

**`Input`'s EOF is the one error main.py renders two different ways**, and no single
payload can carry both. Caught, `:359-360` hands `catchx` `str(e)`, so the guest sees
`EOF when reading a line`; uncaught, the same exception is not a `RuntimeError`, so
`format_error` falls to its last arm and prints `EOFError: EOF when reading a line`.
The port therefore carries a **sixth kind, `CF_EOF = 6`**, holding `:360`'s bare
message, and `format_uncaught` adds the class name from the kind. It is the only kind
that is not control flow - it rides alongside the five rather than inside them - and
`TryCatch` absorbs it exactly as it absorbs `ERROR`, because a guest catching an EOF
must get the message and not a kind it cannot name. `g12` freezes both renderings and
`I05` pins the split.

**Delegation beat transcription for `Input`, and the reason generalises.**
`inputx(p)` is itself an expression the *host* evaluates (`:521-524`), so the port can
call it in its own source and run the host's `input()` directly - the same trick
`eval_file_builtin` uses for `readx`/`writex`. **No native was added**, `interpreter.py`
was not touched, and every `:NNN` citation in this document still points where it did.
An earlier draft of this milestone proposed adding an `input_line` native for exactly
this; that was rejected, because a native would have shifted every downstream line
number in a file whose entire value is that its citations are checkable.

### 12.3 `this`, `super`, and the four receiver shapes of `.`

`eval_dot` (`:596-623`) dispatches on the *receiver's* kind, in order: module (funcs,
then classes, then lets, `:599-606`), instance (fields, then the class chain,
`:607-615`), super-box (`:616-622`), else error. Method lookup searches **fields
first**, so an instance field named like a method shadows it (`:608-609`) - an
ordering nobody would guess and everybody would "tidy up".

`this` and `super` are env lookups guarded by `exists_in_chain` (`:479-489`), so their
errors are about *scope*, not values: `'this' can only be used inside a method or init`
when the chain has no `this` at all, and a different message when `super` exists but is
null (`:487-488`). Bare `super(...)` calls the nearest `init` **above the defining
class** (`:862-871`), which is why `super` carries the defining class and not the
receiver's class (AGENTS.md's note on multi-level chains). The port's super box is
`["P", instance, klassNode]` - the same three fields as `:886-890` - and case `I7a` is a
three-level chain where each level's method calls `super.m()`, which is the shape that
breaks if the klass is wrong.

---

## 13. Milestones, gates, and what each one costs

### 13.1 The unit of cost, so the plan can be read as a plan

Everything below is priced in the one measured unit: **13.6 ms of executed MountX per
unit of oracle-shaped work** (2.2), and the **x10,433 layer factor** (2.4). The two
numbers that are not obvious from either:

```
interp.mx's own imports           host-side, ~10 ms     the PORT's Import statements are
interp.mx's stdlib parse          host-side, ~0 ms      executed by the HOST, which parses
                                                        lexer.mx/parser.py-style, natively.
guest's imports                   port-side, 13.6 ms x  the GUEST's importx runs the port's
                                  nodes imported        own readx + parser.mx (12.1).
guest's own source parse          port-side, 13.6 ms x  measured: fbtest = 3,829 nodes =
                                  guest nodes           52 s exactly.
```

So the anchor's 65 minutes decomposes as 52 s entry parse + 123 s stdlib parse +
3,390 s interpretation = 59 minutes, and the remainder is `display`, prints and the
driver. **A micro case costs about 10 seconds**, which is what makes an inner loop
possible at all: the 26 cases of 14.1 are a 4-minute gate, and with the 44-program message
sweep attached, about 12 - not an overnight run.

### 13.2 Milestones

| # | deliverable | gate | measured cost | decision it answers |
|---|---|---|---|---|
| **I-M0** | `interp.mx` skeleton: boxes (`3.1`), `display`, `type_name`, `is_num`, the kind slot | unit self-test, guest-free: the port prints its own box renderings and they diff against a golden file | seconds | are the value rules (`3.2`, `3.3`) transcription-correct in isolation? |
| **I-M1** | env chain + the first slice of both chains: 11 of 16 statements, 4 of 17 expressions, `handle_import`, `run_program`, main.py's uncaught renderer. **No calls.** Shipped narrower than this row first said, and it gained imports | `--exec`: 5 guest programs run twice, stdout **and exit code** byte-compared against a frozen oracle side; plus I05's 43 self-test rows | ~1.6 s each | `I4` (undeclared-assign), `I12`/`I13` deferred with the loop kinds |
| **I-M2** | the exec core: `callFn`/`evalCall` and the four callee shapes, arity, the depth counter, `If`/`While`/`Loop`/`ForIn`, `BinOp`/`UnaryOp`/`Index`/`This`/`Super`, the module arm of `DotAccess`, the file builtins, seventeen native guards. 15 of 16 statements, 15 of 17 expressions; **the recursion probe run through the port** | 8 guests (140 golden lines) + I06's 62 rows + the bisected depth measurement, which replaced 8.2's derived 498 and moved 8.3's constant twice | ~3 s each, 25 s the stage | the ladder is **7** deep, not 5; the ceiling is 275 calls on a plain body and falls to 66 at nineteen nested expressions and 36 at thirty-nine, so **both** 400 and 100 were inert and **40** is what ships |
| **I-M3** | `TryCatch`/`Throw` and the error paths: all 44 raise-paths. **Shipped as the error pass** - the channel was already standing, so the work was the 38 non-`eval_call` sites (24 swept by a guest, 8 forged-pinned, 9 to I-M4, 1 `D-1`) and the 30-program message sweep | `I1`-`I3` (`I3` is `s30`; `I8a`/`I8b`/`I8d` are `s29`/`s27`/`s28`, `I8c` is `g2`), `I11` deferred to I-M4, plus the 30-program message sweep | 30 guests, 173 exec lines, 70 s the exec stage + 9 s the value layer | does the channel design survive the six interactions? **Yes - and the sweep found two defects the earlier stages could not** (a bare-global `mk_super` collision, and `D-1`'s search half) |
| **I-M4** | classes, `this`, `super`, `New`, `DotAccess` (instance + super arms), `Input`, `ClassDef` in all three of its sites, instance field assignment. **Shipped**: `ClassDef` registers and nothing else (`:341-342`), `eval_new` in the host's order (resolution *before* args, parent-first fields, nearest init), `eval_dot`'s two remaining arms with fields before methods, and `Input` **delegated** - `inputx(p)` is itself a host expression, so no native was added. `too_far` is **gone**: it had five callers and all five are now transcriptions, and 7.1's sixteen statement kinds plus 7.2's seventeen expression kinds are complete | `I5`-`I7c`, `I11`; the 9 class-bound texts as `s31`-`s42`; the class guests `g9`-`g13` | 57 guests, 218 exec lines, 110 s the exec stage; I05 at 55 rows, I06 at 73 | does the class layer survive transcription, and does `MAX_GUEST_DEPTH = 40` still sit below every cliff now that a guest level can cost THREE activations? **Yes - measured**: the class ladder's physical cliff is ~275 levels against the counter's 39, so the counter still wins by ~7x. Three defects the earlier stages could not have found: the `ClassDef` edit that dropped an `if`'s brace (a whole-file parse failure), the EOF rendering split (a payload cannot be both `str(e)` and `<class>: <msg>`), and the module-class overwrite trap in `g11` |
| **I-M5** | loops and the sweep closure. **Shipped as the closure pass**: `While`/`Loop`/`ForIn` and `If`/`else` were already transcribed in I-M1/I-M2, so what this milestone owed was PROOF and the two unmeasured shapes - `g16` (loop-in-loop-in-tryx, return from depth), `g17` (loop-heavy recursion), `g18` (the else-as-a-LIST shape of parser.py:174-179) - plus `s45`-`s47` freezing 25 native-guard and stateful-refusal texts, and **`reach_audit.py`**, which turns 11.3's "42 messages" into a number the gate recomputes | `I5`-`I7c` (closed I-M4), the sweep closure, `g16`-`g18` | 63 guests, 270 exec lines; audit 68 frozen + 3 unreachable of 71 | **Is any message untested?** No - and the three that no program can reach each carry a measured reason rather than an excuse. The loop ladder also narrows 8.3's margin from ~7x to **~4x** (cliff ~157 vs the plain ladder's 275), the first shape to come near making 40 look like a margin |
| **I-M6** | **the anchor**: `python3 main.py selfhost/interp.mx fbtest.mx` | **ACHIEVED: 365 bytes, 23 lines, byte-identical to the oracle, exit 0, stderr empty, md5 a3621b5a0a69c6f71d467c459cac4989** | **193 s measured, against a 57-72 min prediction - ~18x faster; the prediction is WRONG and 2.2/13.2's cost model is unvalidated for this workload (see selfhost/anchor/README.md)** | **is phase 3 correct? Yes, on the anchor.** Needed a BOOTSTRAP LAYER first (13.2.0) - the command was a one-second green until then |
| **I-M7** | self-interpretation. **BOTH LAYERS SHIPPED.** Layer 2: `interp.mx` interpreting `interp.mx` is byte-identical to the host, exit 1, 270 s. **Layer 3 (the crown): `interp.mx` running `interp.mx` running `layer3.mx` is byte-identical, exit 0, 332 s, 23 bytes.** The anchor is unaffected throughout (365 B, 23 lines, byte-identical). Two defect classes found, neither a transcription: argv inheritance across the delegation boundary (fixed by injection, 13.2.0a) and **unbox-before-guard ordering** (fixed by a `len` type arm plus a quiet adapter unbox, 13.2.0b) | `s48` (injection agreement), `s49` (error class), `s50` (the guard audit, 17 natives, and the abort row) | **the 2.2-hour prediction is wrong again**: the crown is 332 s. A third instance of 13.2's unvalidated cost model | Does the interpreter survive being its own guest? **Yes, at three layers.** |

I-M6 is the phase. Everything before it is a case-level check that costs less than a
coffeewalk, and everything after it is a demonstration that cannot be a regression gate.

### 13.2.0 The bootstrap layer: the one section of interp.mx that is not a transcription

`interp.mx` is a transcription of `interpreter.py` from its first line to a marked line
near the end, and **everything below that line is not**. The section is headed

```
---- BOOTSTRAP DRIVER (not a transcription — the port's main()) ----
```

and it is the only top-level statement in the file. Calling it a transcription would be
a lie the file's own comments exist to prevent, so it is labelled, and the label is
load-bearing: it is the boundary a reviewer can diff in one place.

**Why it has to exist.** The host's entry point is `main.py`, in Python, and it does
three things a library cannot reach from inside the language: read `argv`, read the
file, and choose an exit code. A self-hosted interpreter that wants to be *run* has to do
those three things itself. Every self-hosted compiler and interpreter has exactly this
layer — a `main` that boots the thing the rest of the file defines — and it is always
the part with no counterpart in the system being ported.

**What it is not.** It is not a copy of `main.py`. `parse_args`, the REPL, `-e`,
`--version` and `--help` live on the host side of this experiment and are deliberately
out of scope: the port is an interpreter, not a distribution. What the driver reproduces
is the narrow contract the anchor needs — `mountx <guest.mx>` runs a file and exits 0
or 1 with the uncaught line on stdout.

**The argv shift is the trap, and the driver is the odd one out.** The two ways the port
runs see *different* argv, which was measured rather than reasoned about:

| invocation | `sys_args()` | guest index |
|---|---|---|
| `main.py selfhost/interp.mx fbtest.mx` (the anchor) | `["fbtest.mx"]` | **0** |
| `main.py run_probe.mx selfhost fbtest.mx` (the corpus) | `["run_probe.mx", "selfhost", "fbtest.mx"]` | 2 |

`main.py`'s `run_file` consumes `sys.argv[1]` as the script, and `sys_args()` is
`argv[1:]` (`:697-701`) computed *after* that — so in the anchor the port's own path is
already gone by the time the driver sees it. `run_probe.mx` skips three entries; the
driver skips **zero**. Getting this wrong does not produce a subtle wrong answer, it
produces `no guest program specified` and exit 1 in one second, which is how it was
found.

### 13.2.0a I-M7: the layer boundary, and the one native a layer cannot delegate

Two layers work. Three did not, and the reason is the most interesting thing I-M7
found, so it is written down rather than fixed quietly.

**A delegated native answers the HOST's context.** `sys_args` is delegated like every
other native (`:697-701`), so the port answers it from `g_guest_args` - a register the
*parent* layer filled. At two layers that is correct by accident: only one layer consults
it. At three, the inner `interp.mx` reads its PARENT's argv, boots the wrong file, and
the resulting failure surfaces as `Object of type MxInstance is not JSON serializable`
- because the path it read was not a path.

**The general rule, which is the deliverable: a delegated native answers the HOST's
context, so a self-hosting layer must inject its own.** The injection is one slot, one
assignment and one ask:

| where | what |
|---|---|
| `eval_call` (`:1648`) | ONE name steps aside from natives-before-environment: if the env holds `sys_args`, that function is called. Every other native keeps the invariant - `func len` still cannot shadow `len` (I15). |
| `bootstrap_main` | builds a **real MountX function** named `sys_args` returning this layer's resolved guest args, and defines it into the env the guest will run in. |
| `run_program` | one `or`: use the parked env if the bootstrap layer left one, else its own. That is the only line of the transcription that knows the bootstrap layer exists. |

The injected body returns the guest's **own args**, not its path - the same list the
host's `sys_args` would return for it (`:697-701`). A guest never sees its own filename,
in the host or here, and an injection that included it would be a NEW divergence
invented to fix an old one. This is why the two-layer case stayed byte-identical rather
than merely working, and `s48` pins that agreement on both sides.

**A second defect the crown exposed: the error CLASS was assumed from the call site.**
The driver's parse-failure branch called `format_syntax` unconditionally, so a *runtime*
refusal surfaced during preparation wore a `[Syntax]` label. main.py gets this
distinction free from Python's exception classes; the port has one channel for both
(5.8) and has to ask the message. `render_parse_or_runtime` now does, following
main.py:38-40 - a message already opening with `[` is the parser's or the lexer's and
goes out untouched, anything else gets `[Runtime] `. `s49` pins both arms.

### 13.2.0b I-M7.1: the guard audit, and the second defect class

The crown's remaining failure was **not** the argv inheritance. Instrumenting it found a
different thing, and the difference matters: this is a **sequencing defect at the
delegation boundary**, not a transcription omission.

**The mechanism.** The host checks each native's guard on the RAW argument
(`isinstance` / `is_num` on `args[0]`, `:679-680` and eleven siblings), so every message
NAMES the type it was handed. The port **unboxes before the guard** - 9.6's ordering, and
it is right for the adapter - and an unboxable value unboxes to `null`. A guard that then
tests the unboxed value answers for `null` instead of the real type, and the host's own
native raises a *second*, wrong message that clobbers the port's.

**The smallest failing case is TWO layers, not three**, which is what makes it serious:

```
main.py _toklen.mx            -> [Runtime] len expects a string or array, got Token
main.py interp.mx _toklen.mx  -> [Runtime] len expects a string or array, got null
```

**The audit, measured rather than read.** Every guarded native was handed a `Token`
instance - the unboxable case - on both sides. Seventeen rows, one gap:

| native | host guard (cite) | port guard | gap? |
|---|---|---|---|
| `len` | arity `:677-678` + **type `:679-680`** | arity only | **YES - fixed I-M7.1** |
| `time_now_ms` | arity `:685-686` | arity `n != 0` | no |
| `sleep_ms` | arity + `is_num` `:691-692` | `n != 1 || !raw_is_num` | no |
| `sys_args` | arity `:698-699` | arity `n != 0` | no |
| `sys_exit` | arity + `is_num` `:704-705` | `n != 1 || !raw_is_num` | no |
| `os_env` | arity + `isinstance str` `:710-711` | `n != 1 || !raw_is_str` | no |
| `os_cwd` | arity `:716-717` | arity `n != 0` | no |
| `os_listdir` | arity + `isinstance str` `:722-723` | `n != 1 || !raw_is_str` | no |
| `os_sep` | arity `:731-732` | arity `n != 0` | no |
| `mx_version` | arity `:737-738` | arity `n != 0` | no |
| `math_floor` | arity + `is_num` `:742-743` | `n != 1 || !raw_is_num` | no |
| `idiv` | arity + 2x `is_num` `:748-749` | `n != 2 || !raw_is_num` both | no |
| `db_open` | arity + `isinstance str` `:758-759` | `n != 1 || !raw_is_str` | no |
| `db_close` | arity + `is_num` `:771-772` | `n != 1 || !raw_is_num` | no |
| `db_exec` | arity + `is_num` + `isinstance str` `:784-785` | same three | no |
| `db_query` | arity + `is_num` + `isinstance str` `:806-807` | same three | no |
| `http_get` | arity + `isinstance str` `:832-833` | `n != 1 || !raw_is_str` | no |

**Why the sixteen are safe is the interesting part, and it is not a design.** Their arms
test `raw_is_str` / `raw_is_num` on the **unboxed** value, and an unboxable value
unboxes to `null`, which fails both predicates - so their message survives by luck. `len`
was the one native whose arity arm tested nothing else, so its `null` sailed through to
the host's own `len`. **That is a fragile property, not a guarantee**: any future guard
that names a type has to read the BOX, which is why the port now passes the boxed args
to `native_guard` alongside the unboxed ones, and why `s50` is the audit made executable -
a new guard added to `interpreter.py` without a row there is invisible to it, which is
stated rather than hidden.

**The lesson, and it is the second defect class in phase 3.** The first was
transcription; this is **ordering across the delegation boundary**. A transcription can be
checked row by row against the host, but a SEQUENCE - unbox, then guard - is only
observable where the two interact, and the interaction needs a value that exercises both.
A `Token` instance is such a value, and nothing in the corpus had ever handed a native an
unboxable one.

**What I-M7 did NOT establish (superseded by I-M7.1).** The three-layer crown still did not
run when I-M7 shipped: the argv
injection is correct and proven (both layers now read the right guest, measured), but a
further defect remained downstream, in the path where the port interprets `parser.mx`
interpreting a program. It is localised to that path and **not** diagnosed; it is not
the argv inheritance this section is about. Layer 2 - `interp.mx` interpreting itself -
runs byte-identical, and the anchor is unaffected.

**I-M7.1 closed that path, and the crown RUNS.**

```
MOUNTX_STD=<repo>/stdlib python3 main.py selfhost/interp.mx selfhost/interp.mx selfhost/layer3.mx
exit 0 | stderr empty | 332 s | 23 bytes
hello from layer 3!
42                                    byte-for-byte identical to `main.py selfhost/layer3.mx`
```

Three interpreters deep: the host runs the port, the port runs the port, and the
innermost port runs `layer3.mx`. Both artifacts are kept beside the anchor's.

**One more thing the fix required, and it is the half the guard table cannot see.** The
`len` type arm alone was NOT enough. `unbox_args` ran BEFORE the guard and set
`CF_ERROR` with the port's own refusal message, so the guard's correct message was
overwritten - and worse, the signal had already ABORTED the parse, which nothing
downstream can undo. So the adapter's unbox is now **quiet**: `unbox_quiet` yields null
and lets the guard own the message, while `unbox_val` keeps its loud refusal for
`eval_file_builtin`, where 9.4's rule (refuse rather than write a lie) is the point.

That is what the second mutation control found, and it is the sharpest result of the
milestone: with the `len` arm in place, **reverting the quiet unbox SURVIVED the whole
corpus** - every guard row still matched, because the guard overwrote the message. The
corpus was green on a defect that still killed the crown. The row that pins it is
`len([tok])`: the host answers `1` and continues, a loud port aborts before the next
print. **A message that matches while the program is dead is not a pass** - and no
message-shaped assertion could have found that, only a row that checks the program is
still alive.

**What this makes true.** 13.1's I-M6 row says the anchor is
`python3 main.py selfhost/interp.mx fbtest.mx`. That command is now *literally* true: it
is not a description of what the port would do if it had a main, it is the command that
runs the thing. Before the bootstrap layer it was a command that ran nothing and exited
0 - a one-second green, which is the most dangerous kind of green there is, and the
reason this section exists.

### 13.2.1 I-M1 as shipped, and where its three probes live

I-M1 changed the shape of the gate. Through I-M0 there was no guest, so the diff was
between two renderers of one value inside one program. From I-M1 a case is a **program**,
in `selfhost/exec_cases/`, and `parity_interp.py` runs it twice - once through `main.py`,
once through `selfhost/run_probe.mx` - and compares stdout **and the exit code** against
the frozen oracle side (`golden/<name>.exec` + `golden/<name>.exit`). The exit code is a
golden field because `main.py`'s uncaught path is a status *and* a line on stdout
(`main.py:42-52`), which is why `interp.mx` transcribes that renderer
(`format_uncaught`, `format_syntax`) instead of dying quietly. `g2_control.mx` finishes
with a `break` outside a loop precisely so that path is a frozen byte and not a rumour.
`--exec` runs the stage on its own; a plain `--check` runs both stages.

The five guests measure 53 lines and the stage costs about 8 s. It has teeth: mutating
`envSet`'s fall-through to declare in globals instead of the innermost frame turns
`g5_blocks.mx` red on exactly the line that claims it (`Undefined variable 'fresh'`
becomes `7`), and reverting turns it green.

Three probes needed a home, and all three are rows of
`selfhost/test_cases/I05_env_exec.mx` - 43 checks, port-internal, `SPEC:`-stamped except
where a host primitive really is the left column:

| probe | rows | why it cannot be a differential |
|---|---|---|
| the `[parent, dict]` box and the six constructors of 4.2 | `env-box-is-two`, `global-has-no-parent`, `block-env-parent-is-the-env`, `iteration-env-parent-is-the-env`, `module-env-parent-is-globals`, `catch-env-parent-is-the-env`, `catch-env-binds-its-name`, `catch-env-does-not-leak-out` | the oracle's `Environment` is a Python object with two slots; nothing at the MountX surface shows its shape |
| 4.2's sixth constructor and 4.3's stamp | `call-env-no-stamp-is-globals`, `call-env-stamped-is-module-scope`, `stamp-is-a-field-write` | reading a node's `__mod_env__` is port-internal; the *behaviour* half - a module's let staying private - is differential, and it is `g4_import.mx`'s `Undefined variable 'HELPER_LET'` line |
| 5.8's storage rules (V1, V2's emptied-slot claim, V3's readable null), and :258-261's module guard | `return-value-stored`, `return-value-emptied-behind-you`, `return-flag-cleared`, `payload-null-after-clear`, `slot-empty-after-clear`, `null-box-is-readable`, `module-func-cannot-stomp-a-module` | the slot has no guest-visible surface until there are calls; I04 says the same about the kinds |

I05 also holds eight refusal rows - `Unknown stmt Zzz` plus the `too_far` text for
`ClassDef`, `Input` and `New`, and the pair that I-M2 added when `DotAccess` grew its
module arm (`refuses-instance-fields`, `refuses-super-methods`) - so a refusal that
changes wording is a diff, and the one genuinely differential env row that needs no call: the host catches
`Undefined variable 'x'` as a string and the port writes the same words into the slot,
compared by `check_txt` with both sides made at run time. I-M1's version of this file
also refused `If`, `Loop`, `BinOp`, `Call` and `DotAccess`; those rows are gone because
the kinds are not refused any more, and 13.2.2 lists the rows that replaced them, because
a golden row deleted without a successor is how coverage quietly leaves a gate.

Deferred out of I-M1 on purpose: `If`, `While`, `Loop`, `ForIn`, `ClassDef`, and the nine
unimplemented expression kinds - which is why `I12` and `I13` (loop counts, `for...in`
shapes) are still open, and why `I11` is affected by V3's finding that a missing node
field dies in the host rather than in the port.
**Everything in that sentence except `ClassDef`, `New` and `Input` shipped in I-M2; `I12`
and `I13` are g7 rows now.**
**The order is deliberate: I-M0 through I-M5 must be green before the anchor is started
once**, because at 65 minutes a run, iterating on the anchor is not a workflow.

### 13.2.2 I-M2 as shipped: what landed, what the probes said, what the battery killed

I-M2's row above reads "callFn, evalCall, returns, arity". What it took to make calls
mean anything was the rest of the value-and-control layer, because a call whose arguments
cannot be `a + b`, whose body cannot `while`, and whose callee cannot be a module
attribute is a call nobody can write a program with. `interp.mx` went from 1,553 lines to
2,504, and the port now executes **15 of the 16 statement kinds and 15 of the 17
expression kinds**. What still goes through `too_far` is `ClassDef` (statement and
module), `New` and `Input` - I-M4's and I-M5's backlog, stated by the code rather than by
a table. `I11`'s shape did not change: a missing required field is still a host-level
crash, and V3's finding still stands.

| shipped in I-M2 | statements | expressions | where the rows live |
|---|---|---|---|
| calls | `Return` absorb in `callFn`, arity, the counter | `Call` with the four callee shapes (`Ident`, `DotAccess`, `Call`, `Super`) and `'x' is not callable` | `g6_calls.mx` (53 golden lines), `I06_call_exec.mx` |
| control flow | `If`, `While`, `Loop`, `ForIn` | - | `g7_loops.mx` (27 lines), incl. `I12`'s four counts and `I13`'s four shapes |
| the value layer the call needed | - | `BinOp` (13 operators, `EQEQ`-first), `UnaryOp`, `Index`, `This`, `Super` | `g6`, `I06` |
| dotted reads, one arm only | - | `DotAccess` on modules and module functions; instances and `super` **refuse** | `g6`, `g4`, `I05`'s two refusal rows |
| natives | - | the seventeen delegated calls, `native_guard`, `unbox_args`/`rebox_val`, the file builtins | `g6` (the `readx` catcher row), `I06` |
| recursion | - | `MAX_GUEST_DEPTH = 40`, four restore sites, `set_max_depth`/`guest_depth` for the probes | `g8_recursion.mx` (7 lines), `I06`'s four counter rows |

**The stage cost roughly tripled and is still cheap**: 8 guests, 140 golden lines, 25 s
for `--exec`, 9.6 s for the seven value cases, ~37 s for the gate with `--selfcheck`. The
two heavy guests are `g6` (6.6 s, 245 source lines) and `g7` (4.3 s, 182). A guest's cost now
scales with its *call count*, not its line count, which is the first time the port's own
13.6 ms per visit has shown up in a gate number.

**The ceiling probe is the milestone's real finding and it is in 8.2.** `depth_probe.mx`
bisects the port's physical cliff on six shapes - a plain ladder (275 calls), a ring of
five functions (330), and bodies nesting 9 / 19 / 39 / 79 expressions (115 / 66 / 36 /
under 20) - by lifting the port's own counter with `set_max_depth` and reading the exit
code. The port's ladder turned out to be **seven** frames deep, not the five 8.2 counted,
and the ceiling falls faster than the nesting grows. 8.3's constant moved from 400 to 100
and then, on the I-M2 review's one blocking question, to **40**; the reason is in 8.3
verbatim: a counter
above the physical cliff is not a conservative limit, it is an inert one, and an inert one
lets the Python `RecursionError` be the thing that unwinds the port - past the restore
sites, with `g_depth` left high, catchable by a guest `tryx`. 100 was inert for exactly
that reason: at nineteen nested expressions the cliff is 66.

Two lessons the probe owed the design, both in its header now. **Bisect on the exit code**
- 0 fits, 1 is a physical overflow, 2 is a guest that failed to *parse* - because the
widest generator carried 18 openers against 19 closers in a typed literal, so the shape
the design called "what pushes the cliff lowest" had never once run, and stdout prints a
line either way; the generator builds its nesting with `nest(n)` now. And **a column
derived from an assumption is not a measurement**: the old "implied host frames per level"
column divided 10,000 by the measured ceiling and called the quotient a rate, which is how
a ~1,800-frame reality got quoted as 36.4 frames to three figures.

`g8_recursion.mx` runs its deepest ladder at 24 and its mutual recursion at 30 frames,
not near the limit of 40: it sits in the row of 8.3's table where *both* sides run, and a
golden may only freeze that row - at 41 the port refuses where the host runs, so the old
`even_p(41)` and `ladder(40)` lines froze `D-4` as though it were agreement. What the refusal
looks like is `I06`'s `counter-text` row, with the port's limit lowered to 6 by
`set_max_depth` so a forged ladder reaches it in milliseconds - the same trick `I15`'s
shadowing needs, because `func len(x)` is not legal source (the lexer gives `len` its own
token and `parser.mx`'s `parse_funcdef` eats an `IDENT`), which is why 14.1's `I15` row
was unreachable and is now a forgery.

**The mutation battery, run against the shipped port.** Twelve mutants, each a one- or
two-line edit, each reverted (a thirteenth, deliberately cruder probe, is the last
paragraph's). Rows three and thirteen were re-measured after 8.3's limit moved to 40 and
`g8` was dialed back with it; the outcomes are the same, the details are not.

| mutation | result |
|---|---|
| natives checked after the environment | `g6` DRIFT, exit 1 |
| arity message reworded (`expects` → `requires`) | `g6` 3 lines + `I06` 2 rows |
| the depth decrement at the normal return removed | `g6` and `g8` DRIFT, exit 1 - the leak reaches the limit inside `fib(15)`, so `g8` answers a refusal after its first line where the golden has six more; `I06` reads `10` into `counter-restores-depth` and then dies as `PORT-ERROR` when the leak reaches the limit mid-case |
| `call_env` ignores `__mod_env__` (4.3's stamp dropped) | `g6` loses the module-let lines; `I06` 2 rows |
| `while` stops absorbing `break` | `g7` DRIFT, exit 1 |
| `for...in` binds the value to both names | `g7` DRIFT (`0a1b2c` → `aabbcc`) |
| arguments evaluated right to left | `g6` `side:ab` → `side:ba` |
| `BinOp` evaluates its right operand first | `g6` `side:ab` → `side:abb` |
| `Index` evaluates the index before the array | `g6` DRIFT, one `side:` line |
| `Assign` walks an index target before the value | `g6` DRIFT, one `side:` line |
| `evalArgs`' early return on a live signal removed | **survives** |
| that *and* the `Call` arm's post-args check removed | **survives** |
| **`eval_new` merges fields child-first** (12.2's `:653-656` order reversed) | `g9` and `g11` DRIFT - I-M4 |
| **`eval_new` evaluates its arguments before resolving the class** (`:638-648` vs `:645`) | **survives**, and the gap is real - see the note below - now `s43` |
| **bound methods cached** (I-D5 violated, so `obj.m == obj.m` prints `true`) | `g10`, `g11`, `g13` DRIFT - I-M4 |
| **`eval_dot` looks up methods before fields** (12.3's `:608-609` order reversed) | **survives**, same reason - now `s44` |
| **`CF_EOF` rendered as a plain `ERROR`** (drops the class name from an uncaught EOF) | `g12` DRIFT, exit 1 - I-M4 |
| `while` stops absorbing `BREAK` | `g7` and `s30` DRIFT, exit 1 - I-M5 |
| `for...in` binds the element to BOTH names (12.2's `:393-394`) | `g7` DRIFT - I-M5 |
| the `loop` count check accepts a float (`is_num` instead of `is_int`) | `g7` DRIFT - I-M5 |
| `exec_if` unwraps a one-element `else if` instead of running `exec_list` | **equivalent** - see below - I-M5 |

**The two I-M4 survivors are the milestone's most useful result, and they failed for one
shared reason: no earlier program had both sides of the order observable.** `eval_new`'s
resolution-before-arguments rule (7.4's fifth site) needs a program whose class *and*
whose argument both fail - every class case until then had a good class or a good
argument, so swapping the two steps changed nothing a golden could see. `eval_dot`'s
fields-before-methods rule needs a field and a method **sharing one name** - every other
class case reads a field and calls a method under different names, so the order was
invisible there too. Neither is a hole in the port: both were transcribed correctly, and
both mutations were only ever going to be caught by a case written to make the order
observable. That is the same lesson I-M2's two survivors taught, and it is now written
into the milestone: **a transcription of an ORDER is untested until some case puts both
orders in conflict.** `s43` and `s44` are those cases, and both were added *because* a
mutant survived - which is the only honest reason to add a case, and the only way to
know it is not decoration.

**I-M5's one survivor is a different animal, and the distinction matters.** Unwrapping a
one-element `else if` list instead of running `exec_list` over it looks like it should
change scoping - `exec_list` is what pushes a block env (`:377`). It does not, and the
reason is that the unwrapped `If`'s own `then`/`else` branches each call `exec_block`,
which pushes an env anyway, so the only difference is one extra frame nobody can see.
Checked rather than assumed: a `let` inside an `else if` body is still invisible
afterwards on both sides. **An equivalent mutant is not a coverage gap** - it is the
corpus telling you the two spellings are the same program, which is a different and
much less alarming answer than I-M4's two survivors, where a real order was untested.
The test that distinguishes them is not more cases; it is asking *why* the mutant
survived before adding anything.

Ten killed, two equivalent mutants, and the two survivors are the milestone's most
interesting result. `g6`'s `side:` rows were written *because* of the first survivor: the
battery found the coverage gap, the case closed it, and the mutant still did not die.
That is not a broken case - it is 5.8's design working against itself. The host
propagates failure with one mechanism, a Python exception; the port propagates it with a
value that **every caller has to remember to check**, and `evalExpr`'s `BinOp`, `Call` and
`Index` arms each carry a check of their own, so a failing argument stops a later
argument's side effect in three places at once. A gate that freezes behaviour cannot
distinguish a redundant guard from a load-bearing one, and `I14` should be read with that
in mind: it now proves four orderings (left-before-right, array-before-index,
value-before-target, left-to-right args), but the *stop* among them is proved three times
over and no case can say which check supplied it.

Two findings became code. The first is `g6`'s five `side:` lines, which did not exist
until the right-first BinOp mutant (row eight) walked past them. The second came from a
thirteenth probe outside the twelve, cruder on purpose: evaluate the right operand twice,
keeping the order. That one prints no wrong line anywhere in 140 golden lines - `g6` stays
`ok`, because its `side:` markers come from `evalArgs` and its BinOp right operands are
pure - and it makes `g8_recursion.mx` never finish: the recursive call sits on the right of
`1 + ladder(n - 1)`, so one call per level becomes two and the ladder is 2^n deep long
before depth 24. `parity_interp.py` had **no subprocess timeout** at the time, so the gate
waited for it forever. The variant that evaluates the right operand BEFORE the left is
not the invisible one - `g6`'s error-ordering lines catch it, row eight all over again -
which is the reason this probe had to duplicate the evaluation rather than reorder it.
Both
`run_case` and `exec_run` now go through a `_run` with a generous bound (300 s for a case
and for the oracle, 900 s for the port, against 1.4 s / <1 s / ~6 s measured), and a
timeout arrives as a non-empty stderr, which the existing classifier already names
`PORT-CRASH`. Verified both ways: the shipped port is green in 25 s, and the hanging
mutant reports `timed out after 25s (no exit)` with `PORT_TIMEOUT` dialed down for the
test. A gate that can hang is a gate that gets skipped.

**One convention this milestone set, so it does not rot.** `too_far`'s text is
`milestone 2 does not exec <kind>`, and the number is the milestone that *shipped*, not the
one the missing kind belongs to: it says what the port on this branch cannot do. ClassDef
is I-M4's and Input/New are I-M5's, and all three still say "milestone 2", which is true of
the artifact and would be false of the roadmap. `I05`'s three `refuses-*` rows carry the
string verbatim in their `SPEC:` column, so bumping the number is a two-file edit (the
`too_far` body and those three expectations) plus a re-freeze, and it belongs to the release
that earns it, not to the change that adds a refusal.

### 13.2.3 I-M3 as shipped: the error pass, and the two defects a program found

I-M3's row reads "`TryCatch`/`Throw` and the error paths: all 44 raise-paths". The channel
was standing when the milestone opened - I-M1 shipped `TryCatch`/`Throw` and the five-kind
slot, I-M2 shipped calls - so the 29 native guards were free by delegation and the 6
call-dispatch texts came with the calls. What I-M3 owed was the other 38 sites and, of
those, the ones a guest can reach. Split by reachability, not by line order:

| disposition | texts | where |
|---|---|---|
| a guest program reaches it | 24 | `exec_cases/s01`-`s26`, one tiny program each, uncaught, exit 1 frozen |
| only a forged node reaches it | 8 | `check_txt` rows in `I05`/`I06` |
| needs `ClassDef` or `New` | 9 | I-M4 |
| `Module '{n}' not found. Searched:` | 1 | `D-1`, out of the corpus |

The 24 swept include both `'{}' is not callable` paths and both `Index {i} out of range`
paths, so 26 programs carry 24 texts - 11.3's 44-paths / 42-texts arithmetic one milestone
early. The sweep is 30 programs: those 26, three top-level renderings (`s27`-`s29`:
`return`, `continue`, uncaught `throwx`), and `s30`, 14.1's `I3`. `g2_control.mx` already
carries `I8c` (top-level `break`), and `g6_calls.mx` already runs four of the six
call-dispatch texts, which is why the dispatch table is now pinned by the sweep rather than
repeated. `s30` is the one behavioural row: a `return`/`break`/`continue` crossing a `tryx`
inside a `while` inside a function, where the catcher must see the `throwx`/error arm and
never a control signal - 5.4's "the catcher checks the kind" and the host's
`except CONTROL_FLOW: raise` (:355), proven on the port's side by a program rather than by
a forged slot read.

The eight forged texts are pinned because the language will not write them: a parsed
`BinOp`/`UnaryOp` always carries one of the 13 real ops (`Unknown binop`, `Unknown unary`),
an assign target is always `Ident`/`DotAccess`/`Index` or the parser refuses it
(`Bad assign target`), `Expected Program node` and the statement/expression else-arms need
a node the parser never builds, and `'super(...)'` needs a `super` name that is not a super
box, which no method can produce (`:485` fires first from any real one). `I05` and `I06`
forge each of them.

**Two defects the sweep found, neither visible before a program tried the path.**

The first is a bare-global collision - AGENTS.md's own hazard - **inside the port**.
`interp.mx` defines `mk_super(inst, klass)` (3.1's super box) and *imports* `parser.mx`,
which defines `mk_super(ln)` (the Super-node builder). A module function's bare call
resolves through `globals`, and `interp`'s definitions land after `parser`'s, so the
two-arg box stomped the one-arg builder: **no guest containing `super` could parse**.
`s03_super_expr.mx` reported `[Syntax] Function 'mk_super' expects 2 args, got 1` where the
oracle answers `:485`. The lexer and parser gates never saw it, because each loads only its
own port; the interp gate never saw it, because no prior case wrote `super`. Fixed by
renaming the box `mk_superbox` (the 35 `mk_*` builders are parser.mx's, this box is
interp.mx's), and I03/I05's rows move with it. The lesson is the one AGENTS.md already
states: a module must not name a function the way a module it *imports* names one.

The second is the **search half of `D-1`**, which 12.1 had recorded as message-only. Base 4
is the oracle's `interpreter.py` directory - the repo root, where `stdlib/` lives - and the
port's `selfhost/`. With no `$MOUNTX_STD` set (the gate sets none, and neither does
`run_probe.mx`), a guest `importx io` is **found by the oracle and not by the port**: not a
message, a decision. So the exec corpus imports only its own `libs/`, and every stdlib
reaching case - the anchor included - must supply `$MOUNTX_STD` (base 1), which the port
reads first and the anchor's run is expected to set. That is a corpus rule, not a fix; if a
fix is wanted it belongs to I-M5, where the loader is the deliverable.

**Two transcriptions corrected, neither a new capability.** 11.4 requires the port's
`evalExpr` else-arm to set `ERROR` with the host's own `Unknown expr type: {t}` (:531); it
said `Unknown expression {t}` instead, and the `Input`/`New` refusals had subsumed the arm,
so a forged kind reported a milestone where the oracle names the kind. The arm now separates
the two refusals and ends in `unknown_expr`, which speaks the host's text. And `:417`
(`Cannot assign to field of non-instance`) was a `too_far` refusal although a legal program
reaches it - `let a = 5; a.x = 1` - so the DotAccess assign arm now writes the host's
message, keeping its M4 refusal only for the `O` tag a guest cannot build.
`s26_assign_field.mx` is the row; `I06`'s `unknown-expr` and `unknown-binop`/`unknown-unary`
rows are the forged ones.

### 13.3 The layer-3 arithmetic, written out so nobody re-litigates it

At layer 3 the outer port interprets the inner port, so every inner step pays the factor
again. Per guest statement, from the host's 7.4 microseconds for a 5-visit statement:

```
layer 1   7.4 us          layer 2   77 ms          layer 3   805 s = 13 min per statement
plus the guest parse:     6 nodes/statement x 13.6 ms = 82 ms at layer 2 -> 14 min at layer 3
a 5-statement demo        ~0.4 s                   ->  ~2.2 hours
interp.mx parsing its own source (8,000 nodes)     ->  36 years at layer 3
```

**One statement costs thirteen minutes; the interpreter parsing its own source at layer 3
is a thirty-six-year operation.** I-M7 is therefore a 4-6 statement program run once, and
the phase's honest claim about bootstrapping is in 13.4, not in a number nobody can wait
for. This is not a phase-3 failure; it is the wall the phase measured and designed
around, and it is why I-M7 is a demo and I-M6 is the gate.

### 13.4 The bootstrap question, answered without hand-waving

"Does the interpreter run itself?" has three different answers depending on what is
meant, and they should be stated separately because they are usually conflated:

| claim | true? | evidence |
|---|---|---|
| `interp.mx` can interpret a program that is *itself an interpreter* | **yes, at layer 3**, for programs of a few statements | I-M7 |
| `interp.mx` can interpret `interp.mx` on a real program | **no**, not on this hardware: 13 min per statement, 36 years for its own parse | 13.3 |
| the phase demonstrates a fixed point of the *design* | **yes**: the same AST that `parser.mx` emits is consumed by `interp.mx`, and `interp.mx` running a MountX program that runs MountX programs is exercised at I-M7 | I-M6 + I-M7 |

The interesting honest version of the third row: at layer 3 the inner port's *own*
natives are still the host's natives, so `interp.mx` interpreting `interp.mx` exercises
the port's `exec`/`evalExpr`/`callFn` chain as guest data - the interpreter's own
instruction stream passing through the interpreter's own dispatch, with the kind slot
nested six levels deep. **That is the claim worth making, and it needs six statements,
not six thousand.**




---

## 14. Register: the cases, the divergences, the decisions

### 14.1 The corpus, as a list of assertions

Each case is a small guest program plus the assertion that both sides print the same
thing. Sized by 11.2's coverage gap, priced by 13.1 at roughly 10 s each.

| case | asserts | milestone |
|---|---|---|
| `I1` | a thrown value reaches a same-activation `catchx` with its type intact (string, number, array, instance) | I-M3 |
| `I2` | a throw from three frames deep, caught at the top, then a `let` in the catcher landing in the right env (5.3's env-restore obligation) | I-M3 |
| `I3` | `return` inside `tryx` inside `while` inside a function - a guest catcher never sees a non-`THROW` signal | I-M3 |
| `I4` | `x = 1` with no prior `let` declares in the *innermost* env, not globals (4.1) | I-M1 |
| `I4d` | a dotted call whose attribute does not exist reports `Module 'g' has no attribute 'x'`, not an `Undefined variable` about either half - the callee half of 7.5's four shapes | shipped I-M2 (g6) |
| `I5` | cyclic array and cyclic instance print `[...]` / `{...}`; a structurally equal copy does not (3.3) | **I-M0, shipped** |
| `I6` | `A extends B` + `B extends A`, and a three-cycle: `Inheritance cycle involving class 'A'` (11.3) | **I-M4, shipped** - `s33` |
| `I7a` | three-level `super.m()` chain - the shape that breaks if `klass` is the receiver, not the definer | **I-M4, shipped** - `g10` (`L3>L2>L1`) |
| `I7b` | `obj.m == obj.m` prints `false` (12.2, decision I-D5) | **I-M4, shipped** - `g10` end to end, plus three box-level rows in `I06` (two accesses, two stamps, and the `<method peek>` rendering that makes the `false` about identity rather than about two different methods) |
| `I7c` | three instances of one class; mutating one field of one changes only it (field initialisers per instance) | **I-M4, shipped** - `g9`, with BOTH halves explicit: a scalar field mutated on one instance, and an array field reassigned on one instance while the other keeps its own |
| `I8a`-`d` | the four top-level renderings: uncaught `throwx`, top-level `return`, top-level `break`, top-level `continue` (6.1) | I-M3 |
| `I9` | the harness classifies a deliberately crashing port as `PORT-CRASH`, not as a diff (6.2). Its stderr arm fired for real on 13.2.2's hanging mutant once the timeout existed | I-M2 |
| `I10` | `sys_args()` inside the guest equals what the host gives the same program (6.3) | **I-M6 prep, shipped** - the harness gained `EXEC_ARGS` and hands both sides the same tail; `g20` prints the LIST rather than the count, because a count cannot see 6.3's one-argument shift |
| `I11` | a missing required node field is a crash, not a null (11.4) | **I-M4, shipped** - two `I05` rows. It is assertable in the value layer rather than only as a frozen crash because the failure is a `RuntimeError` the host raises while running the port, so MountX's own `catchx` receives it: the rows pin the host's text AND that the port's kind slot is still clean afterwards, which is what separates a loud crash from a swallowed error |
| `I12` | `loop -1`, `loop 0`, `loop 2.0`, `loop true` (7.2) | I-M1 → shipped I-M2 (g7) |
| `I13` | `for` over an array the body reassigns, `for i, v in`, `for c in "abc"`, and `for` over a number | I-M1 → shipped I-M2 (g7) |
| `I14` | evaluation order at 7.4's sites, observable through which error a program reports. **Shipped as g6's four orderings** (`New`'s site is I-M4's), and the shape is not what this row first said: each pair is one failing side plus one side-effecting side, because `error_at` overwrites and two failing sides print the same words in either order | I-M2 |
| `I15` | a user function named `len` does not shadow the native (7.5). **Unreachable from legal source** - the lexer gives `len` its own token - so this is a forged `FuncDef` node in I06, not a program | I-M5 → shipped I-M2 |
| `I16` | `exportx` output compared as **file bytes**, incl. int-vs-float and an unserialisable value (9.4) | **I-M6 prep, shipped** - `g19` writes ten files through both sides and the harness captures and byte-compares them (`FILE_CASES`) |
| `I17` | `writex` / `appendx` bytes, and a `db_query` round-trip through the reboxer (9.3) | **shipped** - `s46` does the `db_query` round trip; `g19` does the `writex`/`appendx` bytes including AGENTS.md's historical glue bug |
| `I18` | negative control: a port edited to skip one check turns the gate red, and the harness's own tests assert that | I-M2 |
| `I19a`-`d` | the four import rules of 12.1, incl. double-import and transitive re-export | **partly I-M2/I-M4** - `g4` and `g11` cover the re-export and module-class paths; the `_imported` once-only rule is in `g4` |
| `I20` | `tryx { readx("/no/such/file") } catchx(e) { printx(e) }` exits 0 with the message (9.6) | **I-M3, shipped** - `s25_read_fail` |
| **sweep** | **all 42 texts of 11.3, one tiny program each (44 paths)**. **I-M3 shipped 30** (24 texts over 26 programs, the 3 top-level renderings, and `I3`); **I-M4 shipped the last 14 programs** - `s31`-`s42` cover the 9 class-bound texts plus three shapes the class layer makes reachable (a field initialiser's env, an init that throws, and a failing field mid-merge), and `s43`/`s44` exist because a mutation SURVIVED: they put `eval_new`'s resolution-before-arguments and `eval_dot`'s fields-before-methods orders into conflict on purpose. 8 forged texts are `check_txt` rows in `I05`/`I06`, and `:488` is one of them: a super box is bound only when the defining class has a parent (`:883-890`), so **no legal program can reach `:488`** - `:485` is what a parentless method sees. `:488`'s text is pinned by `I05` instead | **CLOSED at I-M5** - `reach_audit.py` measures it: 71 distinct texts over 73 paths in the whole file, **68 frozen, 3 unreachable, 0 untested**. See 12.0 |
| **anchor** | `fbtest.mx`, 23 lines byte-identical, exit 0 | I-M6 |

**Where the list stands after I-M2.** Closed: `I4` (I-M1), `I4d`, `I12`, `I13`, `I15` and
four of `I14`'s five sites (all in g6/g7 or I06), `I5` (I-M0). `I9`'s classifier got its
first real exercise from 13.2.2's hanging mutant, once the harness could see one. `I18`'s
substance was run - by hand, twelve times, in 13.2.2 - but the row also asks for the
*harness* to assert it, and that automated half is still open. Half-shipped: `I10`, which
is the argv register of 6.3 with no arguments reaching the harness yet.

**Where it stands after I-M3.** `I1`-`I3` closed (`I3` is `s30`), `I8a`/`I8b`/`I8d` are
`s27`-`s29` (`I8c` is `g2`), and the message sweep is 30 programs: 26 error texts - two of
them on both their paths - plus the three top-level renderings and `I3`. `I11` moved to
I-M4 with the classes it needs. Still open: `I6`/`I7a`-`c` (I-M4), `I11`, `I16`-`I20`, and
the 10 texts the sweep has not reached - 9 that need `ClassDef`/`New`, and `D-1`'s one,
which is a corpus rule rather than a missing case. 13.2.3 is the disposition table.

The message sweep is 44 programs, not 44 written by hand at the end: each is written
the moment its message is transcribed, in the same commit, because a sweep assembled
after the port passes is a sweep that finds nothing (phase 2, `P-M4`).

Three rows were paid for early, in I-M0, recorded here so nobody writes them twice. `I5`
is `selfhost/test_cases/I02_identity.mx`: both cycle renderings, and the shared-sibling
and structurally-equal-copy halves the row asks for. `I9`'s classifier is `classify` in
`parity_interp.py`, keyed on stderr and exit code rather than on message text, and it was
run against a case built to die. `I18`'s negative control ran too - thirteen mutations of
`interp.mx` and its check helper, each turning red exactly the cases that should notice and
no others. Among them: an integral float that lost its `.0`; a stamp counter that stopped
counting; a kind renumbered; `signal_line` frozen at 0; a display tag rendered from the wrong
slot; a string quote flag dropped; an array join that lost its space; `is_num` starting to
accept arrays; the return pair never set; `check_spec` un-stamped; and `display` accepting a
`super` where `D-6` says it must refuse. The two controls that a mutation *should* miss - a
no-op edit, and a lie about a number no case renders - are reported in the milestone notes,
because a gate that only ever turns red is a gate nobody tested.

Every golden row also says which side its expectation came from, because that column can be
one of two things and a diff cannot tell them apart. An unstamped row's second field is the
host's answer, produced at run time: `"" + v` - the PLUS rule at `interpreter.py:551-552`, the
same `display()` that `printx` calls at `:310` - or `typeofx(v)`, the native's own answer over
the raw value. A stamped row prints `SPEC:` first and can only be written through
`check_spec`, because there is no host surface to ask: 5.8's kind slot is invisible to a guest
program, and `D-6`'s refusal text is something the oracle cannot reproduce. Thirty-four of the
132 frozen lines are stamped - 33 in `I04`, the `D-6` refusal in `I03`. `I04`'s eleven unstamped
rows are all differentials: two ask `typeofx` what a cleared payload is, two ask the host's
renderer what a payload string looks like, and seven hand the host a value literal - the array
a `throw` carries, that array's `type_name`, the array in the return pair, and the forged value
three ways - so its renderer and type test write the second column. That last family is the
rule worth stating: an expectation about a structure written as a string is a guess about
quoting, and written through the host it is an answer. The convention is here because a literal
dropped into an unstamped column makes the same field mean "the oracle says" or "I say"
depending on which row a reader is on, and the golden would not show which. `interp_check.mx`
is inside the diff, so the stamp cannot be quietly removed from the helper: that mutation is
one of the thirteen above, and it turns `I03` and `I04` red together.

### 14.2 Divergences, accepted and bounded

| # | divergence | why it cannot be closed | bound |
|---|---|---|---|
| `D-1` | the module search list **and which modules exist**: `Module 'x' not found. Searched: [...]` names `interpreter.py`'s directory in the oracle and `selfhost/` in the port | base 4 is the oracle's `interpreter.py` directory (the repo root, where `stdlib/` lives) and the port's `selfhost/`; faking the list would be a lie in a message, and the exec gate sets no `$MOUNTX_STD` | two halves. **Message**: no corpus case imports a *missing* module. **Search**, found by I-M3's sweep: a guest `importx io` is found by the oracle through `stdlib/` and **not** by the port, so no exec case imports a stdlib module - every case imports only its own `libs/`, and a stdlib-reaching run (the anchor) supplies `$MOUNTX_STD`, base 1 |
| `D-2` | uncaught non-`RuntimeError` Python errors print `[Runtime] TypeError: ...` in the oracle, `[Runtime] ...` in the port | `main.py:50` renders the Python class name; the port's `ERROR` payload is a string (9.6) | guest-visible half matches (`catchx` sees `str(e)` both sides); corpus does not `exportx` an unserialisable value uncaught |
| `D-3` | a guest that parses at depth (an `importx` inside recursion) overflows where the host does not | the port's parse ladder is ~4 levels per construct and shares the 10,000 frames | corpus rule: no `importx` below depth 4 (8.2) |
| `D-4` | guest depth 41 up to the shape's cliff succeeds on the host and fails on the port - the cliff is 330 calls for a five-function ring, 275 for a plain ladder, 115 at nine nested expressions, 66 at nineteen, 36 at thirty-nine | the limit has to sit below the cliff of every shape that could still finish something; above it the Python `RecursionError` unwinds the port instead of the counter (8.3, 8.4) | a band of 26 to 290 calls, shape-dependent; at and past the cliff the message is identical; the corpus's deepest ladder is 24 (`g8`), 60% of the limit |
| `D-5` | a port crash is exit 1 with a traceback; the oracle's exit 1 is a formatted message | phase 3 does not touch `main.py`, so it cannot add an exit code | classified by stderr, and `I9` proves the classifier (6.2). Corrected by measurement in I-M0: an error the interpreter catches prints on stdout, exit 1, stderr empty - unreadable files too - so `parity_interp.py`'s `classify` keys on stderr *or* a nonzero exit, never on message text |
| `D-6` | `printx(super)` inside a method prints the host's `str(dict)` - a `ClassDef` node repr, memory address included - where the port refuses and dies loudly | the oracle's half is not reproducible bytes: the address differs between two runs of the same program, so nothing can be frozen and nothing can be transcribed | a corpus rule, not a golden line: no case prints a bare `super`. Reachable only inside a method of a class with a parent. `typeofx(super)` = `dict` *is* reproducible and is checked (`I03`) |

Five of the six are message-only or out of the corpus by construction, and `D-1` is the
one whose *search* half is already semantic - a `stdlib/` import found by the oracle and
not by the port until `$MOUNTX_STD` supplies base 1 (13.2.3). `D-6` is the real
exception, and it arrived with I-M0: it is semantic - the two sides really do print
different things - and uncloseable in a way none of the others is, because the
oracle's own half cannot be repeated between two runs. What cannot be reproduced
cannot be frozen, so its bound is a corpus rule plus a loud refusal instead of a
golden line. Read the rule with that hole marked in it: a difference only *the port*
has is a defect, and a difference the oracle cannot reproduce either is a `D-n` with
its corpus rule written down beside it.

### 14.3 The decisions, each with the alternative that lost

| # | decision | the alternative, and why it lost | section |
|---|---|---|---|
| `I-D1` | one global kind slot, six kinds (I-M4's EOF is the sixth), read once where a branch is needed | carry control flow on the port's own `throwx` with a tag - **less code, and unsound**: the tag is guest data, so `throwx ["PORTCF","RET",nullx]` would forge a return | 5.8 |
| `I-D2` | transcribe both dispatch chains in the host's order | reorder for speed - buys 0.1% and costs the side-by-side audit that makes 104 behaviours checkable | 7.1 |
| `I-D3` | `MAX_GUEST_DEPTH = 40`: below the cliff of every shape whose ceiling is above 40, and five times fbtest's deepest recursion | the derived 400, and then 100 - both **inert**, because a body that nests nineteen expressions cliffs at 66, under 100, and the physical stack then dies inside the port, past 5.3's restore sites, catchable by a guest `tryx`; a limit at or below 36 would close the heaviest shapes' escape but refuses `y`-shaped guests between 37 and 66 that the port completes; matching a cliff exactly is a coin flip (8.4); `setrecursionlimit` is a host change nobody can afford | 8.3, amended by I-M2's measurement and again by the I-M2 review |
| `I-D4` | delegate all 17 natives + 4 file builtins; determinism policy lives in the harness | port the effect-free seven - three new bug classes (`idiv` int-ness, `math_floor` round-trip, O(n) `len`) for no measured gain | 9.2 |
| `I-D5` | stamp bound methods per access, no cache | cache them - would print `true` for `obj.m == obj.m`, making the port more consistent than the language | 12.2 |
| `I-D6` | one `tryx`, in `callNative`, with unboxing outside it | let Python exceptions propagate, or wrap the whole call path - the first loses a passing test (`I20`), the second lauds port bugs into guest values | 9.6 |

Nothing here is a preference. Each row is a choice that one of the cases in 14.1 can
falsify, which is the only kind of decision worth recording.

---

## 15. Epilogue - what this document corrected

### 15.1 Fifteen claims from earlier in the phase that the reading or a mutation disproved

Every one of these was recollection rather than a stale document: `PARSER_DESIGN.md` and
`DESIGN.md` were checked and contain none of these citations. Each row is the claim, the
check that killed it, and where the correction landed.

| claimed | what is actually true | check | landed |
|---|---|---|---|
| `self.traceback`, `MX_TRACEBACK`, a renderer at `main.py:69-79` | **no traceback machinery exists anywhere**; `main.py:69-79` is `run_file`'s read block | `grep -ri traceback` over 85 files: zero hits | gate deleted, 6.4 |
| `printx` is a native | it is a **statement kind**, `interpreter.py:309` | read `execute` | 9.1 |
| `num_to_string` at `:819-843` | no such function; `display` falls through to `str(v)` at `:88` | read `display` | 3.3 |
| `values_equal` at `:860-891` | `:533-547` | read | 3.2, 7.3 |
| `call_function` at `:823-857` | `:874-902` | read | 7.5 |
| `main.py` is 148 lines | 177 lines | read it | header |
| `interpreter.py` is 932 lines | `wc -l` says **931**; the last line has no trailing newline | `od -c` on the tail | header |
| the anchor asserts ~10 error messages exactly | it asserts **zero**, and has **one** `tryx`, at `fbtest.mx:189` | grep | 11.2 |
| the anchor calls `time_now_ms` and needs a clock normaliser | **zero** clock/`os_cwd`/`os_listdir`/`sys_args` calls; `random` is seeded with a literal 42 | grep | 10.3 rewritten, normalisers deleted |
| "25 error messages to transcribe" | **44 raise-paths / 42 texts**; 29 native guards are free | AST walk of every `RuntimeError` call, bucketed by enclosing function | 5.5, 11.1 |
| "38 messages, and all 35 sites inside `eval_call` are free" | 6 of those 35 are **call dispatch**, not natives, and must be transcribed | same AST walk, read line by line | 11.1, 11.3 |
| arrays must be `count + dict` for append cost | plain MountX arrays: `push` is `arr + [v]` anyway, and phase 2 already builds AST lists this way | `array.mx:202`, `parser.mx:742` | 3.1 rewritten |
| `sys_args` is delegated with a one-place argv shift | the port cannot reach the host's argv at all; the driver installs a register the port reads | read `:697-701`; built the shift, and it handed the guest its own filename | 6.3, `set_guest_args` |
| 29 native guards are free because the host raises them | MountX has no splat, so a wrong-arity call never reaches the native to be refused; 17 of the 29 are transcribed | mutated: the port died indexing `a[0]` and 9.6's catcher reported `list index out of range` where the host says `len expects 1 argument` | 9.3, `native_guard` |
| the port's call ladder is five host frames deep, so the guest ceiling is ~498 | it is seven deep, and the ceiling is 275 calls on a plain body, 115 at nine nested expressions, 66 at nineteen, 36 at thirty-nine - it falls toward zero as the body gets heavier, so **no** constant beats it and the limit is chosen by a rule (8.3) | `depth_probe.mx`, bisected on six shapes | 8.2, 8.3, `I-D3` |

Three of the first twelve would have produced **passing gates on code that did not exist** (the
traceback gate), **a normaliser that hid real divergence** (the clock), or **a
representation chosen for a cost the language does not have** (`count + dict`). That is
the same failure family phase 2's `V1` belongs to, and the rule this phase adopts is
cheap enough to have no excuse: *read the lines before citing them, and count before
claiming a number.*

One further correction, from measurement rather than reading: the recursion cliff is
**per call, not per guest nesting level**, at 4.00 Python frames per call, so phase 2's
"2,300 frames" was 2,300 *calls* - a label error that mattered, because it hid the
constant needed to predict the layer-2 budget (8.1).

### 15.2 The measurement log

Every number in this document comes from one of these, all run today, all in `/tmp`
against copies of the tree - no host file was modified:

| artifact | what it measured |
|---|---|
| instrumented `interpreter.py` copy (counters only) | 2.1's node/visit/call/depth/frame tallies for `fbtest.mx` and `tour.mx` |
| `selfhost/parse_repo.mx` on a one-file list | the 52.15 s / 73.4 nodes/s calibration, and that the dump is byte-identical to the oracle's |
| `chain5.mx`, `chain9.mx` bisected to 1 | 8.1's cliff: 2,490/2,495 and 2,484/2,493 calls, 9,983 frames at 4.00 per call |
| two `fbtest.mx` runs diffed | determinism, 10.3 |
| AST-dump kind histograms | 11.2's coverage table, incl. `Super` x2 / `Continue` x1 repo-wide |
| `selfhost/measure_raises.py` - AST walk of every `RuntimeError` call, bucketed by enclosing function | 11.1's 73 = 29 + 6 + 38 split, the 42 distinct texts, and the two shared ones. Committed, unlike the rest of this log, because 104 behaviours is load-bearing |
| `selfhost/depth_probe.mx`, bisected on six ladder shapes (committed - the artifact 8.2's table rests on, and whose widest generator had never parsed until the I-M2 review's question found the unbalanced literal) | the 7-frame ladder, and the 275 / 330 / 115 / 66 / 36 call cliffs that moved `I-D3` from 400 to 100 and then to 40 |
| the 12-mutant battery of 13.2.2, plus its thirteenth probe, applied and reverted against the shipped port | which g6/g7/I06 rows have teeth (10 killed), the two equivalent mutants 5.8 explains, and the missing subprocess timeout the harness now has |
| `parity_interp.py --exec` timings, re-measured after every case change | 140 golden lines in 24-27 s; the first gate numbers in which the port's 13.6 ms per visit is visible (g6 6.6 s, g7 4.2 s) |

### 15.3 What phase 3 changed outside `selfhost/`, and why it was allowed

Two changes, both requested before this document was written, and one of them did touch
a host file:

1. **`AGENTS.MD` -> `AGENTS.md`.** The references now read `AGENTS.md` in `README.md:9`,
   in `interpreter.py:22`'s version comment, and in the two design docs. **That second
   one is a change inside a host file** - a comment line, no code, no behaviour, and the
   file still runs the same - so the honest version of "phase 3 touches no host file" is
   *the port adds no host-file change; one comment line was renamed earlier in the
   phase.* Recorded here rather than buried, because the differential's whole value
   rests on the oracle being untouched, and anyone auditing `interpreter.py` later
   should not have to wonder whether this phase edited it.
2. **`fbtest.mx`'s scratch file moved to `$TMPDIR`**, via a `tmp_path()` helper
   (`fbtest.mx:48-56`) modelled on the one `tour.mx` already had. It previously wrote
   `mountx_final_test_tmp.txt` **into the repository**, because the gates run with
   `cwd=ROOT` - which is precisely how phase 3's own differential could have fed a stale
   file back as input. Verified after the change: stdout byte-identical to the pre-change
   run, `OK - 277 check(s) passed`, zero FAILs, and nothing left in the tree (re-checked
   just now: `mountx_final_test_tmp.txt` does not exist).

This is a preview of the argument phase 4 will need.

### 15.4 What phase 4 inherits

- A working `interp.mx` certified by the anchor, and **a measured layer factor** to
  replace 2.4's x10,433 estimate - probably the single most useful number phase 4 gets,
  because it turns every future "can we run X at layer n" from a guess into arithmetic.
- `MAX_GUEST_DEPTH` and the real port ladder depth from I-M2, replacing 8.2's model.
- The delegation adapter (9.3) and its one catcher (9.6) - the boundary every future
  capability has to pass through, and the place where "no new natives" is enforced.
- A differential harness with a negative control that demonstrably fails, which is the
  only kind of harness worth handing to a phase whose subject is interpreters.
- The two things phase 3 deliberately did **not** do and someone will eventually want:
  a dispatch trampoline for depth (8.5) and a bound-method cache (I-D5). Both are
  correctness-relevant, not just performance, and 12.2 says exactly which observable
  each one breaks.
