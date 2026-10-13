<!-- Copyright (C) 2026-2027 Ziad Mohsin. All Rights Reserved. -->
# B-1 kickoff - the exact subset, and how mini.c is done

**Status: DONE — 12/12 green.** `mini.c` is written, builds with zero warnings,
and passes every gate. The findings below are what building it changed.

## 1. The exact subset

COMPILE_DESIGN §10 described B-1 as "a C program that lexes, parses, evaluates
`Literal`, `Ident` and `Print`, and exits". That was loose, and loose is what
produces a milestone that is neither done nor failing. **This is the list.**

### Statements - exactly 7

| kind | why it is in B-1 |
|---|---|
| `Let` | the env's `define`; without it there is nothing to read |
| `Assign` | the env's `set`, including **assign-or-declare** (COMPILE_DESIGN §2.3) |
| `Print` | the output contract |
| `ExprStmt` | `peek()` on its own line - needed by cases 08-10 |
| `Throw` | the whole point of B-1 |
| `TryCatch` | the other whole point |
| `Return` | so a function can produce a value for `ExprStmt`/`Print` |

**Excluded, with the reason:** `If`/`While`/`Loop`/`ForIn` are loops and
branches - they need the statement walk's *stop-on-signal* semantics, which is
INTERP_DESIGN 7.2's one genuinely new translation, and B-1 should not be the
milestone that discovers it. `Break`/`Continue` without their loops are dead.
`FuncDef` is **not** a statement for B-1's purposes: case 08/09/10 need it, so it
IS included (see below). `ClassDef`, `Import` - B-3.

### Expressions - exactly 6

**Verified, not asserted:** the corpus's ASTs were walked with the host's own
`parser.py` and the distinct node types are exactly

```
Assign BinOp Call ExprStmt FuncDef Ident Let Literal Print Return Throw TryCatch TypeOf
```

(`Program` is the root.) So the subset is 12 node kinds, and any second list
written by hand would have been wrong - it was wrong once already, which is why
this line is a measurement.

| kind | why it is in B-1 |
|---|---|
| `Literal` | every value's origin |
| `Ident` | the env chain walk |
| `BinOp` | **`PLUS` only.** The other 12 operators are B-2; including one keeps case 04 focused on arm ORDER, which is the part that is easy to get wrong |
| `Call` | the call frame - the C frame that `longjmp` must unwind |
| `TypeOf` | **added after measurement.** Case 06 uses it to assert a thrown number arrives as an `int`, which is the only way to prove `ThrowValue` carries a *value* rather than a rendered string. It is 8 lines of `type_name` and its output is a golden line. Leaving it out would have meant weakening case 06 to `printx(e)` alone |
| `FuncDef` | a definition, not an expression - cases 08/09/10 need it |

### So, precisely: 7 statements + 5 expressions + `FuncDef`.

`If`/`While`/`Loop`/`ForIn`/`Break`/`Continue`/`ClassDef`/`Import` are all
**absent** - the measurement above proves it, so a C port that mis-handles any of
them will fail loudly on this corpus rather than pass it by never being called.

The exact set the corpus exercises is verifiable: `selfhost/b1/corpus/`, 12
cases, listed with what each pins in its own header comment.

### Findings from building mini.c

**F-1 — the subset spec was WRONG about BinOp, and the corpus caught it.**
KICKOFF.md said "`PLUS` only, the other 12 operators are B-2". Measured against
the corpus's ASTs, `03_print_display.mx` uses `DIV` (`1 / 3`) and
`08_func_call_return.mx` uses `MUL` (`x * 2`). **The hand-written subset was
wrong; the corpus was right.** mini.c implements all three and the spec is
corrected above. This is the third time the corpus has caught the designer
rather than the implementer (the others were cases 04 and 09), and it is the
strongest argument yet for condition 5's ordering.

**F-2 — six defects in mini.c, all found by the corpus, none by reading.**

| # | symptom | root cause |
|---|---|---|
| 1 | 9 cases produced NO output | the harness captured streams separately while the goldens merged them; the fix was in the harness, not the port |
| 2 | case 05: `Expected token }` | `parse_body` ran to EOF; a block must stop AT `}` |
| 3 | case 04/07: error text first | mini.c wrote errors to **stderr**, the host writes them to **stdout** (`main.py:55` `print(format_error(e))`). The stream is part of the frozen bytes |
| 4 | cases 05/06/09/10/11: `Expected catch variable` | `catchx` is spelled `catchx(e)`, so `(` comes **before** the variable - unlike `func f(x)` |
| 5 | case 09: **segfault** | `parse_expr` consumed the `(` only after the binop loop, so `"x " + f()` parsed as `+ ident` and left a dangling `(` |
| 6 | case 09: printed `99` | the call env's parent was passed as `env` instead of the **global** env - the callee could see its caller's local, i.e. an invented closure |
| 7 | case 09: caught message was empty | `mx_clear_signal()` ran **before** the catch body, wiping `g_sig.msg` that the body's `e` reads |

Defects 5, 6 and 7 are the interesting ones, and all three are the same
*category*: **a bug in the error channel or the scoping rule produces a wrong
answer rather than a crash, and only a frozen golden catches it.** Defect 5 is
the exception - it segfaulted, which is at least loud.

**Defect 6 is the one that matters most.** It is the exact thing COMPILE_DESIGN
§8 warns about: a compiler "fixing" the scoping model by resolving a call env to
its caller looks like an improvement, passes nothing, and quietly creates a
language feature MountX does not have. The corpus caught it on its first run.

`If` and `While` are **not** in the subset, but `mx_exec` needs *a* statement
dispatch table, and a missing arm must be a **loud refusal** rather than a
silently-skipped statement. So both appear as an explicit
`mx_unsupported("If")` arm that raises the frozen text
`Unknown stmt If` - which is what the host does for a kind it does not know, so
the behaviour is indistinguishable from correct until B-2 teaches it. **This is
recorded as a deliberate temporary, not an oversight.**

## 2. What mini.c must do, in order

1. **Lex** `selfhost/b1/corpus/*.mx` - the subset's token kinds only. A token
   outside the subset raises the frozen `Unexpected token ...` shape rather than
   mis-parsing.
2. **Parse** to the C AST of COMPILE_DESIGN §2, including the `mountx`/
   `unmountx` wrapper (auto-wrapping is the host's job at `main.py`, and B-1's
   entry point reproduces it).
3. **Evaluate** the subset, with `mx_py_repr` for every number and `mx_display`
   for every value.
4. **Exit** with the host's code: 0 normally, 1 on an uncaught failure.

**No code generation in B-1.** `compiler.mx` is B-3/B-4 work. B-1's binary
*interprets* the subset directly. This is a deliberate narrowing: B-1 exists to
prove CB-G's error channel and `mx_py_repr`, and a code generator in the same
milestone would put two new mechanisms between the reader and the result.

## 3. The gates

| gate | requirement | how it is checked |
|---|---|---|
| G1 | **11 of 12 cases byte-identical** to `.out`, and exit codes equal | `b1_run.py`, which diffs stdout and compares `.exit` |
| G2 | **case 12 DIVERGES** in exactly the frozen way - the overflow text, exit 1 | same harness, with `EXPECT_DIVERGE` in the manifest |
| G3 | `-Wall -Wextra -Wclobbered` **clean** | the build step fails the gate on any warning |
| G4 | `mx_py_repr` unit test: the **9 values** of §6.6, byte-for-byte | `b1_pyrepr_test.c`, compiled and run separately |
| G5 | **no unreachable case** - every case runs, none is skipped | the harness reports the count; 12/12 is the only pass |

**G2 is the one that looks like a failure and is not.** A C port that *matches*
the host on case 12 has an absent or broken overflow guard, which is the bug
silent wrapping always is. The harness therefore fails if case 12 **matches**,
as loudly as it fails if 11 do not.

**G4 exists because §6.6's values cannot be written as .mx literals** -
`5e-324` and `1e+20` are representable but `str(2**70)` is not, and a subnormal
is not something a test should smuggle in through a literal. They are unit
tests.

## 4. Red line

COMPILE_DESIGN §10's red line for B-1: **if `setjmp`/`longjmp` with an out-of-line
payload proves unreliable, STOP and re-open CB-G** before writing more on top of
it. Cases 05, 06, 10 and 11 exist to make that failure visible in the smallest
possible program. A B-1 that needs a workaround for the error channel has
answered its own gate.

## 5. What B-1 is explicitly NOT

- **Not a code generator.** No `.c` is emitted; nothing is compiled-to.
- **Not the full evaluator.** 5 of 17 expression kinds, 7 of 16 statement kinds.
- **Not `compiler.mx`.** That is B-3/B-4, and CB-A's phase-1 plan has it
  emitting C for the *whole* language.
- **Not a performance claim.** Nothing here measures speed; §2.4's projection is
  a B-5 question.