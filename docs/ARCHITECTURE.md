<!-- Copyright (C) 2026-2027 Ziad Mohsin. All Rights Reserved. -->
# MountX Architecture

How the interpreter is built, what it costs, and where the interesting parts are.

This document describes the **host** (the Python interpreter in
`interpreter.py`) and the **port** (`selfhost/interp.mx`, a transcription of the
host written in MountX). The port is not shipped — nothing in `main.py` refers to
it. It exists to be compared against the host.

---

## Contents

1. [The three layers](#the-three-layers)
2. [The value system](#the-value-system)
3. [The kind slot](#the-kind-slot)
4. [The delegation boundary](#the-delegation-boundary)
5. [The depth budget](#the-depth-budget)
6. [Environments, classes and modules](#environments-classes-and-modules)
7. [The gates](#the-gates)

---

## The three layers

```
source text (.mx)
      |
      v
+------------------+   tokens      +------------------+
|  lexer.py        | ----------> |  parser.py       |
|  TOKEN_SPEC      |              |  recursive       |
|  table of regex  |              |  descent         |
+------------------+              +------------------+
       ^                                  |
       |  selfhost/lexer.mx                |  AST (plain dicts)
       |  (a port)                         v
       +-------------------------|  interpreter.py   |
                                  |  tree-walking    |
                                  +------------------+
                                            ^
                                            |  selfhost/interp.mx
                                            |  (a port)
```

**lexer.py** is driven by a single table of regexes (`TOKEN_SPEC`). Each entry
pairs a pattern with a token type. Tokens carry line and column, which is why
every `[Lexer]` and `[Parser]` message has a position. Comments (`#`) are dropped
at lex time.

The lexer also accepts keyword aliases — `class`/`classx`, `new`/`newx`,
`null`/`nullx`/`nil`, `len`/`lenx`/`length`, `throw`/`throwx` and more. The
standard library uses the `x` forms consistently.

**parser.py** is recursive descent, with this precedence chain, loosest first:

```
or < and < comparison < additive < multiplicative < unary < postfix
```

AST nodes are **plain dicts**, and most carry a `"line"` key. That matters: the
port's job is to produce dicts identical enough to compare, and plain data is the
cheapest thing to compare. `parse_class` stores `init` in its own slot, separate
from `methods` — a structural decision with a bug attached (see
[classes](#classes-and-the-init-slot)).

**interpreter.py** is a tree-walker. `execute()` handles the 16 statement kinds,
`eval()` the 17 expression kinds, and the rest is environments, classes, imports,
natives and exception plumbing.

### Two ports, three files

| File | Role |
|---|---|
| `selfhost/lexer.mx` | a transcription of `lexer.py`, in MountX |
| `selfhost/parser.mx` | a transcription of `parser.py`, in MountX |
| `selfhost/interp.mx` | a transcription of `interpreter.py`, in MountX |

`interp.mx` is the largest and the only one that executes programs. Each is
compared to its original by running the same input through both and diffing the
results.

---

## The value system

### The host: eight kinds, told apart by `isinstance`

The Python interpreter distinguishes values by type check
(`interpreter.py:91-111`). This is ordinary Python and needs no explanation.

### The port: ten boxes, each an array with a tag

MountX has **no `isinstance`**. So the port has to answer "what is this value?"
some other way, and it does it by making every value an array whose first element
is a kind tag (`interp.mx:124-167`, design docs §3.1):

```
[tag, ...payload]

  "N"   number        ["N", v]
  "B"   bool          ["B", true|false]
  "Z"   null          ["Z"]
  "S"   string        ["S", "..."]
  "A"   array         ["A", id, arr]
  "F"   function      ["F", id, funcdefNode]
  "M"   bound method  ["M", id, methodNode, instanceBox, klassNode]
  "O"   instance      ["O", id, className, dict]
  "U"   module        ["U", id, modName, funcsDict, classesDict, letsDict]
  "P"   super         ["P", instanceBox, klassNode]
```

Three rules make this work, and each exists because the alternative breaks
observably.

**Everything is boxed, including numbers and booleans.** An unboxed number is
indistinguishable from an array length or a dict size in a language with no type
test, and the port's dispatch is `arr[0]`. The cost is one array read per value
read; the escape hatch is internal-only and never visible in output.

**Arrays are a stamp plus a plain MountX array — not `count + dict`.** The
host's arrays are Python lists, and MountX arrays *are* Python lists, so wrapping
one gives the host's semantics for free: `arr[i]` and `arr[i] = v` are O(1) and
in place (`interpreter.py:418-427`), `+` copies, `len` is the native, and
**element order is guaranteed**. A `count + dict` encoding would depend on dict
insertion order — the exact hazard being avoided.

**The stamp is an identity for objects, and arrays compare deeply.** Both facts
matter. `next_stamp()` gives every array, function, instance and module a fresh
integer on creation. But MountX's equality rule is *deep for arrays* and
*identity for instances*, so the stamp's visible effect is on instances:

```
let a = [1, 2]
let b = [1, 2]     # same contents
printx(a == b)     # true   - arrays deep-compare

classx P { func init() {} }
printx(newx P() == newx P())   # false - instances compare by identity
```

That split is also why `I-D5` rejected method caching: the host stamps a bound
method on *every* attribute access, so `obj.m == obj.m` is `false`, and a cached
port would print `true` — the port would be more consistent than the language,
which makes it a worse mirror.

### The constructors that matter

```
func mk_null()          { return ["Z"] }
func mk_num(v)          { return ["N", v] }
func mk_arr(items)      { return ["A", next_stamp(), items] }
func mk_superbox(i, k)  { return ["P", i, k] }
```

They are **functions** rather than constants because building a box is the act of
crossing from a raw value into the port's value world. The comment above
`mk_bool` says so explicitly.
---

## The kind slot

Control flow inside the port needs a way to say "stop, this expression returned"
or "stop, an error happened" without using exceptions for everything.

The obvious design is to reuse `throwx` with a tag:

```
# the design that was REJECTED
throwx ["PORTCF", "RET", nullx]
```

Less code, no new variable. It is also **unsound**, because the tag is *guest
data*. A program containing that line would forge a return and jump out of the
middle of the port's own interpreter — the guest would be able to set the
interpreter's control flow by writing ordinary MountX.

So the port uses a dedicated slot a guest cannot reach (`interp.mx:385-402`):

```
func CF_NONE()   { return 0 }
func CF_RET()    { return 1 }   # Return   (:345), absorbed by callFn (:899)
func CF_BREAK()  { return 2 }   # Break    (:348), absorbed by the innermost loop
func CF_CONT()   { return 3 }   # Continue (:350), the same three absorbers
func CF_THROW()  { return 4 }   # Throw    (:361), absorbed only by TryCatch
func CF_ERROR()  { return 5 }   # every error path, absorbed by TryCatch
func CF_EOF()    { return 6 }   # input's EOF - the sixth kind, added in I-M4
```

Seven functions, one variable, read once wherever a branch is needed:

```
eval(...):
    ...
    set_signal(CF_RET(), value, line)
    return value            # the signal is NOT the return value
```

The distinction is the point. A function returns its value *and* may set a
signal; the signal says what the surrounding construct should do.

Each kind has exactly one absorber:

```
CF_RET      -> callFn          CF_THROW / CF_ERROR  -> TryCatch
CF_BREAK    -> innermost loop  CF_EOF               -> the input site
CF_CONT     -> innermost loop
```

`CF_EOF` is the odd one: it exists because `input()`'s EOF is the one error
`main.py` renders in **two** ways — bare `str(e)` when a `catchx` sees it, and
`"EOFError: "` prefixed when uncaught. The port has to carry both forms, so it
carries the bare one and lets the host's formatter do the rest.

---

## The delegation boundary

### What a "native" is

Seventeen builtins cannot be written in MountX because they need to touch the
outside world: the clock, the filesystem, `sys.argv`, the network. The port does
not reimplement them. It **calls back into the host**:

```
guest program running in the port
        |
        |  eval_call -> the 17 natives
        v
callNative(...)  ->  re-invokes the host's own eval_call
        |
        v
the host performs the real work (open a file, read the clock)
```

The decision is `I-D4`, and the alternative was explicitly considered: port the
seven effect-free natives (`idiv`, `math_floor`, `len`, ...) and skip the
delegation. That would have been faster, and it would have created three new bug
classes — `idiv`'s int-ness, `math_floor`'s round-tripping, and an O(n) `len`
where the native is O(1). Delegating cannot drift, because it *is* the host's
dispatch.

### The limit

**A delegated native answers the HOST's context, not the guest's.** That is the
boundary's whole cost, and it is why layer 2 could not work at first.

`sys_args()` returns the *host's* arguments. So a guest running inside the port
would get the host's `argv`, not its own — a wrong answer, produced by correct
code, at a layer nobody had tested.

The fix is **injection** (design docs §13.2.0a): exactly one name, `sys_args`,
steps aside from *natives-before-environment*, so the port can supply its own.
One name, and it is deliberately not a general mechanism.
---

## The ordering, and the quiet adapter

This is the subtlest thing in the port, and it took two layers to find.

The host guards each native on the **raw** argument:

```python
# interpreter.py:677-680
if len(args) != 1:
    raise RuntimeError("len expects 1 argument")
if not isinstance(args[0], (list, str)):
    raise RuntimeError(f"len expects a string or array, got {type_name(args[0])}")
```

There is no unboxing step in the host. The guard *is* the message.

The port used to **unbox first, guard second** — right for the adapter, wrong
here. An unboxable value (a `Token`, say) unboxed to `null`, the port then wrote
an error the host never writes, one step before the guard that owns the real
message. And the signal it set had already aborted the parse.

The smallest reproduction is two layers, not three:

```
main.py token_len.mx           ->  len expects a string or array, got Token
main.py interp.mx token_len.mx ->  len expects a string or array, got null
```

The fix had two parts, because one was not enough.

**Part one** — give `len` the type arm it lacked. An audit of all 17 natives then
found this was the *only* gap in seventeen. The other sixteen are safe **by
luck, not by design**: their arms test the unboxed value, and `null` happens to
fail both predicates. So the port now passes the **boxed** arguments to the
guard alongside the unboxed ones, and any future type-naming guard reads the box.

**Part two** — make the adapter's unbox **quiet**: it yields `null` and lets the
guard own the message. The loud refusal stays where the rule *is* the point, in
`eval_file_builtin`.

Part two was not optional, and this is the part worth remembering. With only part
one in place, reverting to loud unboxing **survived the entire corpus** — every
guard row still matched, because the guard overwrote the message. The suite was
fully green on a defect that still killed the crown.

> A message that matches while the program is dead is not a pass.

What caught it was a row checking the program was still **alive**: `len([tok])`,
where the host answers `1` and continues, and a loud port aborts before the next
`printx`. No message-shaped assertion could ever have found it, because the
message was right.

`selfhost/exec_cases/s50_native_guards_typed.mx` is that audit, executable: all
17 natives handed an unboxable value, both sides, compared — plus the abort row.

---

## The depth budget

MountX has no tail calls and Python has a recursion limit. The port runs on both,
so the question is: **where does the port physically fall over?**

`selfhost/depth_probe.mx` measures it by lifting the port's own counter with
`set_max_depth` and reading the exit code — `0` fits, `1` physical overflow, `2`
the guest did not parse.

```
guest shape                     port's physical ceiling
----------------------------------------------------------
plain recursion ladder              275 calls
five-function mutual ring           330
9 nested expressions                115
19 nested expressions                 66
39 nested expressions                 36
```

The ceiling is a property of the **guest's shape**, not a constant — each
expression nested in a body can cost several activations, so the ceiling falls as
the body gets heavier. No constant is below every cliff, because the ceiling
falls toward zero as a body gets heavy.

`MAX_GUEST_DEPTH` is therefore **40**, and it is a rule rather than a margin.
This is the load-bearing reason:

```
guest recurses past the counter's limit
    -> the port sets CF_ERROR and refuses        (intended)
    -> a clean, catchable error                  (correct)

but if the counter were set HIGHER than the cliff:
    -> the Python stack dies first
    -> RecursionError unwinds the port past its restore sites
    -> the depth variable is left HIGH
    -> a guest tryx catches it
    -> the guest continues from a corrupted interpreter
```

So a limit above the cliff is not merely useless, it is **worse than no limit** —
it converts a loud refusal into a quiet corruption.

Two previous values, both derived rather than measured, were rejected: `400`
(from the arithmetic) and `100` (a re-derivation). Both were **inert** — the
stack died before reaching either. `40` is measured, and it sits below every
cliff belonging to a shape that could still finish something.

There is a corresponding rule for the host: **never raise Python's recursion
limit to make a parity run pass.** The limit is what makes the boundary loud;
raising it hides the divergence instead of removing it, and the resulting green
tick costs more than the red it replaced (register entry `V1`).

One consequence for the test corpus: `g8`'s deepest ladder is 24 and its mutual
recursion 30 frames, because a refusal inside a golden would freeze the depth
---

## Environments, classes and modules

### Environments

An environment is a chain of scopes. `set()` assigns-or-declares, and a call
creates a `call_env` whose parent is the defining function's module environment
(for module functions) or the globals.

```
globals
   |
   +-- mod_scope          (a module's top-level lets - PRIVATE, never globals)
   |      |
   |      +-- call_env   (one per activation)
```

Two invariants here exist because breaking them recreates a fixed bug:

- **`__mod_env__` tagging.** Every module function, method and `init` AST node is
  tagged with `__mod_env__`, so a bare reference to a module-level `let` still
  resolves **at call time**. Without it, a module function called from elsewhere
  could not see its own module's variables.
- **Module top-level lets live in a private `mod_scope`, never in `globals`.**
  If they went into globals, two modules' variables would collide.

### Classes and the `init` slot

`self.classes` maps a name to a class AST. A class node stores `init` in **its own
slot**, separate from `methods`.

This is not a style choice. `init` is not in `methods`, and **all** method lookup
goes through `find_method(klass, name)`, which checks `klass["init"]`
explicitly. When `init` was hidden inside `methods`, `super.init(...)` reported
`super has no method 'init'`.

Inheritance is parent-first field merge with the child overriding, and the nearest
`init` up the chain runs. A subclass that defines no `init` inherits its parent's
— which is why `classx Dog extends Animal {}` constructs correctly.

`super` is a dict carrying the instance and **the class defining the current
method**, not the receiver's class. That is what makes multi-level chains resolve
correctly:

```
classx A { func who() { return "A" } }
classx B extends A { func who() { return "B/" + super.who() } }
classx C extends B { func who() { return "C/" + super.who() } }

newx C().who()  ->  "C/B/A"
                 and super.who() inside B gives "A", because B is the
                 defining class - not C.
```

### Module protection

In `handle_import`'s `FuncDef` branch, a module object must **never** be
overwritten by a same-named function. This is not hypothetical: `arrayfull.mx`
once defined `func map`, which silently destroyed the `map` *module object*, and
the fix was the protection rule.

The same hazard appeared a second time, in a different language: `interp.mx`
imports `parser.mx`, whose 35 `mk_*` builders include `mk_super`, so the port's
own `mk_super` resolved through globals and stomped it. Renamed to `mk_superbox`.

> Never name a stdlib function the same as a module it lives in or imports.
> That is a bare global collision, and there is no warning at the call site.

### Import search order

---

## The gates

Six commands. Each covers a failure mode the others do not.

```
python3 -m py_compile main.py lexer.py parser.py interpreter.py
python3 main.py fbtest.mx                      # 277 assertions
python3 selfhost/parity.py --check             # lexer, 52 curated
python3 selfhost/parity.py --repo --check      # lexer, every file in the repo
python3 selfhost/parity_parse.py --check       # parser, 49 curated
python3 selfhost/parity_parse.py --repo --check# parser, every .mx in the repo
python3 selfhost/parity_interp.py --check      # value layer + 68 guest programs
python3 selfhost/reach_audit.py --check        # 71 message texts, 0 untested
```

**Why six.** `fbtest.mx` exercises the lexer and parser only through programs
that already work, so a grammar edit that changes what a working program means
passes it and is caught nowhere else. The lexer and parser ports exist to close
that hole, and they sweep the **whole repository**, because phase 1's two defects
and phase 2's three were all found in the sweep and none would have been in a
hand-written case set.

**Two stages in the interpreter gate.** Stage one prints the host's rendering of
a value beside the port's, in one stream. Stage two is the real differential: a
program in `selfhost/exec_cases/` runs once through `main.py` and once through
`run_probe.mx` (the port running the port), and **both stdout and both exit
codes** must match the frozen golden.

**Exit codes are compared** because `main.py` prints its uncaught-error line to
stdout and exits 1. A port that rendered the error perfectly and exited 0 would
pass a text-only gate.

**The trigger rule.** Any change to `lexer.py`, `parser.py`, or any
`selfhost/*.mx` re-runs **all five** parity commands, not the one that looks
touched — because `lexer.mx` is lexed by itself and `parser.mx` is parsed by
itself, so both sweeps cover every file in the tree. A change to the interpreter's
`display` or `type_name` re-runs the value layer, because those are the two
functions it transcribes.

### The anchor and the crown

```
MOUNTX_STD=<repo>/stdlib python3 main.py selfhost/interp.mx fbtest.mx
```
byte-identical to `python3 main.py fbtest.mx` — 365 bytes, 23 lines, md5
`a3621b5a0a69c6f71d467c459cac4989`, exit 0, 193 s.

```
MOUNTX_STD=<repo>/stdlib python3 main.py \
    selfhost/interp.mx selfhost/interp.mx selfhost/layer3.mx
```
byte-identical to `python3 main.py selfhost/layer3.mx` — 23 bytes, exit 0, 332 s.

Both artifacts are kept in `selfhost/anchor/`, because a result nobody can re-run
is a story rather than a measurement.

`interp.mx` ends with a **bootstrap driver** — the port's `main()` — which is the
only non-transcription section of the file. Without it the anchor command is a
one-second green that runs nothing, which is exactly what happened first.

### Known divergences

Four differences are recorded rather than fixed, each with a reason: `D3`
(`\uXXXX` only to U+00FF in the port), `D5` (`\w` is ASCII-only in the port),
`D-6` (printing a bare `super` gives the oracle a memory address, so no golden can
carry it), and `V1` (the port's expression ladder gives out near 250 nesting
levels where the host reaches 1,000).

The rule is explicit: **do not silently fix one, and do not fix one by narrowing
the Python so both sides agree.** That is a user-visible behaviour change wearing
a parity fix as a disguise, and it needs its own approval.

---

## Where to look in the code

| Question | Where |
|---|---|
| Why is `is_num` written that way? | `interpreter.py:51-52` |
| How does `==` reach comparison? | `eval_binop` — EQEQ/NEQ/GT/LT/GTE/LTE are handled *before* the "needs numbers" guard |
| What does `len` guard on? | `interpreter.py:677-680`, `interp.mx:677-680` |
| How are values boxed? | `interp.mx:124-167` |
| Where is the kind slot? | `interp.mx:385-402` |
| Where is the depth limit set? | `MAX_GUEST_DEPTH` in `interp.mx`; measured by `selfhost/depth_probe.mx` |
| How does a class find `init`? | `find_method(klass, name)` in `interpreter.py` |
| How are imports searched? | `handle_import` — the `subs` list and the base list |

- **[STORY.md](STORY.md)** — how this all got built.
- **[../AGENTS.md](../AGENTS.md)** — the full invariant list.
For each base directory in order, MountX tries `stdlib/<sub>/<name>.mx` (subs
`""`, `core`, `data`, `structs`, `text`, `math`, `system`), then
`stdlib/<name>.mx`, then `libs/<name>.mx`, then `<name>.mx`. First hit wins.

The bases are deduped and come from:

1. `$MOUNTX_STD`, if set
2. the entry script's directory (`interp.script_dir`, set by `run_ast()`)
3. the current working directory
4. the directory holding `interpreter.py`

A new stdlib category folder must be added to `subs` in `handle_import` — a fact
worth stating because it is invisible from the folder's contents.
rule as though it were agreement.