<!-- Copyright (C) 2026-2027 Ziad Mohsin. All Rights Reserved. -->
# selfhost/parser.mx - design (phase 2: parser only)

Status: **proposal. No code exists.** Phase 1 (`DESIGN.md`) is shipped and
green, so everything below is written against a working port and a working
harness, not against hope. Decisions that need your call are numbered
`P1..P8` and collected in section 18. The phase-1 gate is green as this is
written (`PARITY OK: 52 cases, 255 lines`), which is the baseline P-M1 has to
preserve.

Every number in this document was produced by running something in this tree.
Where a number replaced an assumption, the assumption is named and struck.

---

## 1. Goal

Re-express `parser.py` in MountX as `selfhost/parser.mx`, such that for any
token stream the MountX parser yields the same AST and the same diagnostic text
as `parser.py`. Prove it differentially, with `parser.py` as the oracle, on top
of the phase-1 harness.

Why it is worth doing, beyond "the lexer did it": the parser is where MountX
first has to build and walk a **tree**, which is the capability phase 3 (the
interpreter) actually needs. The lexer proved the host can loop over characters;
the parser proves it can allocate structure, recurse over it, and serialize it
back out byte-exactly. It is also the cheapest place to find out whether the AST
shape in `parser.py` is accidentally Python-shaped.

## 2. Non-goals

- No interpreter, no evaluation, no codegen. Phase 2 stops at the AST.
- No change to `parser.py`, `main.py`, `interpreter.py` or `lexer.py` - and no
  new natives, same rule as phase 1.
- Not a performance play. `parser.py` stays the production parser.
- No `Token` change that alters phase 1's canonical output. The `lexer.mx`
  parity gate must stay green *while* phase 2 is built; section 9 says how the
  parser gets values without touching `canonical()`.
- Not importable from the repo root (`selfhost/` is not in the import search
  path), so the drivers again live inside `selfhost/`. See `DESIGN.md` section 2.

## 3. The parity contract

**A1 - AST equality is a canonical dump, not an object comparison.** One line
per node, pre-order, `|`-separated (section 13). Two implementations are equal
when the byte streams are equal. This is the AST analogue of E1, and it exists
because there is no way to compare a MountX object graph with a Python dict from
the outside.

**A2 - diagnostic equality is at the message level.** E2 carries over
unchanged: `parser.py` raises `SyntaxError`, `main.py` prints it verbatim
because the text starts with `[`, and the MountX side `throwx`es the identical
string, which the harness strips back out. 16 templates, all 16 reached
(section 14).

**A3 - numbers in the AST are compared as values, not lexemes.** This is the one
place where phase 2 deliberately *breaks* E3, and it is not a contradiction: E3
governs the token stream, where `lexer.py` keeps raw text; the AST does not -
`lexer.py:170` has already turned `007` into the int `7` before `parser.py` ever
sees it, so the oracle has no lexeme left to compare. Value comparison is
therefore forced, and it is measured
safe (section 10). If it stops being safe, the fallback is P3 in the register,
not a silent pass.

**A4 - both sides parse the same string, wrapper included.** The interpreter
wraps a headerless module before parsing it (`interpreter.py:233-235`: if
`tokens[0].type != "MOUNTX"`, re-lex as `"mountx\n" + code + "\nunmountx"`). The
harness does the same, on both sides, before lexing. This is load-bearing: the
wrapper shifts every `line` field by one, so if one side wraps and the other
does not, the diff is a million lines and meaningless.

**A5 - explicitly out of scope for equality:** process exit codes, `printx`
rendering of anything not in the dump, Python `repr` of a node object (we dump
our own form), and `py_repr` above U+00FF (D3/D4 carry over - three of the 16
templates interpolate `{value!r}`, and `py_repr` is verified only over 0x00-0xFF).

## 4. The oracle, in one page

`parser.py` is 422 lines: one `Parser` class, one method per construct, plain
dicts out, `parse(tokens)` at the bottom. No precedence table, no visitor, no
node classes. Three ideas:

1. **Cursor discipline.** `current()` reads `tokens[pos]`; `eat(expected)`
   checks-then-advances; `eat()` advances unconditionally. There is **no
   lookahead anywhere in the file** - every decision is a test on
   `current().type`, either `==` a name or `in` an inline tuple, and `peek` is
   defined but never called. MountX has neither optional parameters nor tuples,
   so the port needs two eat methods and a few named membership helpers where the
   oracle has inline `in (...)` (section 9).
2. **Keyword-led statement dispatch** (`parser.py:62-87`): 14 keyword branches,
   then `IDENT`/`THISX`/`SUPER` to the assignment-or-expression path, then a
   20-member `EXPR_STARTERS` set for bare expression statements, then error.
3. **A 9-deep call ladder for expressions** (`parser.py:253-327`):
   `expression -> logical_or -> logical_and -> comparison -> additive ->
   multiplicative -> unary -> postfix -> primary`. Each level is a `while` loop
   that folds `left` into a `BinOp` per operator token.

Five details are where a from-memory rewrite goes wrong:

| detail | parser.py | why it bites |
|---|---|---|
| `lenx(x)` is sugar | `:360` returns `Call{callee: Ident{name:"len"}}`, and the synthesized `Ident` takes the **`lenx` token's** line | there is no `Len` node to compare against; the desugar is visible in the dump |
| `inputx` is optional-paren | `:346-354`: bare `inputx` is legal, `inputx()` gives `prompt = None`, `inputx(p)` gives a node | three shapes, one node type |
| trailing `,` is legal in arrays only | `:415-416` breaks on `RBRACK` right after a `COMMA`; calls (`:311`), `newx` (`:380`) and params (`:224`) have **no** such break | `[1,2,]` parses, `f(1,)` is an error |
| dotted names, two places | `extends dict.Dict` (`:116`) and `newx dict.Dict(...)` (`:373`) fold dots into a **string**, not into `DotAccess` nodes | `New.class` is `"dict.Dict"`, not a tree |
| assignment is a *statement* decision | `:154-168`: parse a full expression first, then look for `=` | four of the 16 diagnostics are about what may sit left of `=`, and they are order-sensitive (`:157` before `:159` before `:161`) |

Also: `parse` seeds an empty token list with a synthetic EOF (`:20-21`), and
`current()` synthesizes EOF past the end (`:26-28`). The seed is reachable
(`parse([])`); the past-end branch is not reachable from a stream that came from
`lex()`, because the lexer always appends `EOF` and no unconditional `eat()`
branch matches `EOF`. Section 9 keeps both anyway, and says why.

## 5. What the AST actually is (measured, not modeled)

Over every `.mx` file in the tree (34 files, wrapped per A4): **33 parse,
13,784 nodes, 34 distinct node types, deepest AST nesting 16** (section 11
explains why that last number is not the one to worry about for recursion). The
one failure is `fbtest.mx`, which contains its own `mountx` on line 5 - it is not
a single program and the wrapper cannot make it one (section 16 turns it into a
free error-parity case rather than an exclusion).

Two findings changed the design, and both came from dumping rather than reading:

**(a) Class fields are not nodes.** `parser.py:129` appends
`{"name": f_name, "value": f_val, "line": f_tok.line}` - **no `type` key**.
48 of them sit in `ClassDef.fields` across the repo. A dumper that dispatches on
`node["type"]` dies on the first class in `stdlib/data/*.mx`, and a port that
hands every child to a generic "it's a Node" routine will happily invent a type
that the oracle never emitted. So the dump carries a 35th shape, spelled `FIELD`,
with exactly two fields, and the port models a field as a pair rather than as a
`Let` node wearing a hat. Related detail from the same method: a field's `ASSIGN`
is mandatory (`class A { let x }` is a parse error), and a field's `line` is the
`let` keyword's line, not the identifier's.

**(b) All 34 key sets are stable across the whole repo.** For every type, the
union of observed keys equals the intersection - no type gains or loses a key
depending on the file. So a fixed per-type field order (section 13) is a spec,
not a guess. The rare types are the only real coverage risk:

| type | count | | type | count | | type | count |
|---|---|---|---|---|---|---|---|
| Ident | 3961 | | Assign | 574 | | ClassDef | 18 |
| Literal | 1978 | | Let | 533 | | Loop | 5 |
| BinOp | 1491 | | This | 478 | | Read | 2 |
| Call | 945 | | If | 448 | | Write | 2 |
| DotAccess | 900 | | FuncDef | 436 | | Super | 2 |
| Return | 587 | | ArrayLiteral | 332 | | Append | 1 |
| Index | 273 | | While | 135 | | Export | 1 |
| ExprStmt | 203 | | Import | 66 | | Continue | 1 |
| Break | 17 | | New | 33 | | **Input** | **0** |
| TryCatch | 13 | | TypeOf | 23 | | Program | 33 |

`Input` is produced by `inputx` and **the repo never uses it**. Everything under
~20 (Append, Continue, Export, Read, Write, Super, Loop, TryCatch, ClassDef) is
likewise thin: the repo corpus cannot certify those branches, so the curated
corpus has to (section 16). Same lesson as phase 1 - a corpus that never emits a
template is either covering dead code or missing a bug.

`Literal` needs its own row, because the dump must render its `value`:
`INT` 854, `STRING` 881, `BOOL` 127, `NULL` 113, `FLOAT` **3** - and those three
floats are `0.5`, `0.5`, `1.0`. Note `value_type` is the **token type**, so a
NULL literal is `{value: None, value_type: "NULL"}` and a BOOL literal is
`{value: True, value_type: "BOOL"}`. A negative number is never a literal:
`-1` is `UnaryOp{op:"MINUS"}` over `Literal{1}`, because `parser.py:297` runs
after the lexer has already split the minus off.

## 6. Where every `line` comes from

The oracle does not use one rule. It uses seven, and on a one-line test program
all seven agree, which is precisely why a hand-written port gets them wrong and
the first corpus run passes. Measured provenance, node by node:

| line is taken from | nodes |
|---|---|
| the leading **keyword** token | `Let` `Print` `Import` `If` `While` `Loop` `ForIn` `FuncDef` `Return` `TryCatch` `Throw` `ClassDef` `New` `TypeOf` `Input` `Read` `Write` `Append` `Export` |
| the token itself | `Ident` `Literal` `This` `Super` |
| the **operator** token, not the left operand | `BinOp` (`:262` etc.), `UnaryOp` (`:300`) |
| the opening delimiter | `Call` = the `(` (`:315`); `DotAccess` = the `.` (`:319`); `Index` = the `[` (`:324`); `ArrayLiteral` = the `[` (`:419`) |
| inherited from the inner expression | `ExprStmt` and `Assign` - `expr.get("line", 0)` (`:86`, `:167`, `:168`); the `=` and the statement's start are irrelevant |
| literal `1` | `Program` (`:60`) - always 1, never the `mountx` token's line |
| the `let` keyword inside a class body | `FIELD` (`:129`) |

Three consequences for the corpus:

- `1 +` then a newline then `2` must be a case: `BinOp.line` is the `+` line, and
  a port that copies the left operand's line passes every single-line test.
- `f\n(1)` must be a case: `Call.line` is the paren's line, which differs from
  the callee's.
- a class field declared on its own line must be a case, so that `FIELD.line`
  (the `let`) is distinguishable from `FIELD.value.line`.

The `, 0` in `expr.get("line", 0)` is dead: every expression node the oracle
builds carries a `line`. The port mirrors the call site with a plain field read
and does **not** invent a zero-default path, because a zero would silently pass
where the oracle could never produce one.

## 7. Host constraints (each one measured, none assumed)

`DESIGN.md` section 7 still applies in full - no closures, no `chr`/`ord`,
quadratic string and array growth, code-point indexing, no negative index. These
are the additional rows that only show up once the job is "build a tree":

| fact | observed |
|---|---|
| optional / default args | none. `eat(expected=None)` must become `eat()` plus `eat_is(kind)`; `peek(offset=1)` likewise, and `peek` is **never called** in `parser.py` - dead code, and section 9 says what to do with it |
| `x is not None` | write `x != nil`; the only real use is the duplicate-`init` check (`:133`) |
| dict / set / frozenset literals | none. `EXPR_STARTERS` (20 names), the comparison-operator set (6), the additive set (2) and `FILE_BUILTINS` (4 entries with an arity each) all become `if`-chains over strings - see the cost row below for why that is not a compromise |
| dynamic field creation | works: `n.value = expr` on a `Node` instance adds the field, read-back correct (probed on this tree) |
| reading a field never assigned | `[Runtime] Field/method 'missing' not found on Node` - a loud error, which is what we want: a port that forgets to set a field dies at the dump instead of silently emitting `null` |
| negative indexing | `tokens[-1]` is an index error, so `current()`'s past-end branch (`:27`, `self.tokens[-1]`) must be written `tokens[len(tokens) - 1]` |
| `body.append(x)` | write `body = body + [x]`; measured 4,000 appends in 36 ms, and the largest node list anywhere in the repo is 152 entries, so the quadratic rebuild is irrelevant here (section 12) |
| `HashTable` get | **~4.5 ms per `get`, measured twice** (2,000 gets = 9.0 s; 20,000 gets = 84 s). Cause is in the library, not the concept: `_hash` calls `_char_code`, which does a nested substring scan of a 95-character table *per character of the key* (`stdlib/data/hashtable.mx:18-60`) |
| dispatch alternatives, same harness | linear scan over the 35 node names: 96 µs per lookup; a 7-branch string `if`-chain: 12 µs; a 15-branch statement-dispatch chain: 17 µs. A chain is **~375x** cheaper than a hashtable get, and the oracle's own shape (an `if` chain) is also the fast shape |
| recursion budget | a 9-deep mutual-recursion cycle survives 250 iterations (2,250 MountX frames) and dies at 275 (2,475 frames). See section 11 - this is the number that killed an earlier design change |
| number -> text for the dump | `"" + 7` -> `7`; `"" + 1.5` -> `1.5`; `"" + string.tonum("0.00001")` -> `1e-05`; `"" + string.tonum("12345678901234567890")` -> exact. The host's `display` is Python `str`, so int and float rendering agree with the oracle **by construction** |
| bool / nil -> text | `"" + true` -> `true`, `"" + nil` -> `null`. These are *not* Python's `True` / `None`, and three error templates need Python's spelling - section 14.2 |
| value conversion available to the parser | `string.tonum` only, and it is decimal-only: `1e3` never reaches the parser because `lexer.py` refuses to lex it (`Invalid number '1e'`), so the port's number table never sees an exponent form |

## 8. Shape of the module

One file, `selfhost/parser.mx`, wrapped in `mountx ... unmountx`, importing
`string` (for `tonum`) and nothing else - no `hashtable` (section 7), no `io`,
no `error`. The driver imports `lexer` and `parser` as siblings.

**Transcription is the default.** `parser.mx` mirrors `parser.py` method for
method, in the same order, so a reviewer can hold the two files side by side and
diff them by eye. Any deviation - a split method, a hoisted helper, a dropped
branch - must appear in the deviation list at the end of section 9. That rule
exists because the parity harness proves *equality of output*, which is weaker
than *equality of structure*, and the day they disagree you want a diff, not an
archaeology dig.

**Data model - two classes, one builder per node type.**

- `Node`: `kind` plus sparse fields, no `init` arguments (MountX `init` cannot
  take optional args and three of the node types have absent fields). 35 module
  functions - `mk_let`, `mk_binop`, `mk_field`, ... - each setting *exactly* the
  keys `parser.py` sets for that type, and nothing else. Nothing else, on
  purpose: a builder that pre-fills a field the oracle would leave unset turns a
  loud `Field not found` into a quiet `null`, which is the one failure mode this
  design cannot afford. `kind` carries the oracle's `type` string verbatim - the
  field is `kind`, not `type`, for the same reason phase 1 spelled a token's kind
  `ttype` - and it includes the 35th spelling `FIELD` for class fields
  (section 5a).
- `Parser`: `tokens`, `pos`, plus 30 methods (the oracle has 32 `def`s; `__init__`
  becomes `init` and `peek` is dropped, section 9). State lives in the object;
  there is no closure to capture it with anyway.

Why one `Node` class and not 34: the dump walks a `(kind, field-list)` table, so
per-type classes buy no dispatch, and 34 `classx` declarations would each need a
builder anyway - the same 35 functions, plus a class-name lookup to maintain.
Decision P1, alternative recorded.

**Public surface** (contract, not code):

| entry | returns | raises |
|---|---|---|
| `parse(tokens)` | the `Program` `Node`, mirroring `parser.py:422` | section 14 messages, as `throwx` values |
| `parse_src(src)` | `parse(lex(src))`, the convenience the interpreter will eventually want | lexer *and* parser messages |
| `parse_print(src)` | nothing; prints the section-13 dump, one line per node, and on error prints one `ERR\|<exact message>` line and stops | - (catches, per A2) |
| `dump(node)` | the dump as one string - for tests and for small inputs only; the corpus path uses `parse_print`, see section 12 | never |

`parse_print` again exists because MountX has no callbacks and a returned
multi-hundred-KB string is quadratic (section 12). It is the same reasoning that
produced `lex_print` in phase 1, and the harness reuses the same diff plumbing.

## 9. The cursor, the dead code, and the value the oracle has that `Token` does not

**Two `eat`s.** `eat()` advances unconditionally; `eat_is(kind)` checks, raises
`Expected {kind}, got {type} ({value!r})`, then advances. The oracle's single
method with a `None` default cannot be transcribed literally, and splitting it
keeps the error site in exactly one place, as it is in the oracle.

**Keep the past-end branch.** `current()` synthesizes an `EOF` token when `pos`
runs off the end (`parser.py:26-28`), and `parse([])` seeds a lone `EOF`
(`:20-21`). The second is reachable today (`parse([])` gives `Program must start
with mountx` at line 1 col 1 - verified against the oracle). The first is not
reachable from a stream produced by `lex()`, because `EOF` is always the last
token and no unconditional-`eat` branch matches `EOF`. Port it anyway:
`parse(tokens)` is public, a caller can hand it an array with no `EOF`, and the
alternative is a `Index out of range` runtime error where the oracle produces a
`[Parser]` diagnostic - a divergence in the one place A2 promises there is none.
MountX has no `tokens[-1]`, so the last-token read is `tokens[len(tokens) - 1]`.

**Leave `peek` behind.** `peek(offset=1)` (`parser.py:31-35`) is never called -
grep finds the definition and zero uses. The port omits it. It is deviation D-1
in the list below, and it is the only omission: everything else has a call site.

**The value gap.** Phase 1's `Token` deliberately keeps the *lexeme*: E3 says the
canonical token stream carries `007`, not `7`, and `Token.value()` returns
exactly that (`lexer.mx:286-295`). But `parser.py` reads `tok.value` *after*
`lexer.py:167-174` converted it - `007` is the int `7` there, and the AST stores
it. So the parser needs a conversion the lexer port refused to do. It goes in
`parser.mx` as `tok_value(tok)`, **not** as a `Token` method, for two reasons:
phase 1's contract about `Token` stays untouched (the `lexer.mx` gate keeps
passing without a rebuild), and phase 3 must not find a conversion method
sitting on `Token` where E3 says the lexeme belongs. The table mirrors
`lexer.py:167-174` line for line:

| token | `tok_value` | note |
|---|---|---|
| `STRING` | the decoded value | `lexer.mx` already has it in `decoded` - no re-decode |
| `INT` | `string.tonum(lexeme)` | exact for any digit run (section 10) |
| `FLOAT` | `string.tonum(lexeme)` | section 10 measured this against `float(text)` |
| `BOOL` | `lexeme == "true"` | yields MountX `true`/`false` |
| `NULL` | **the text `"nil"`** | `lexer.py` never converts `NULL`; the *node* then stores `None` (`parser.py:336`), which is why the diagnostic prints `got NULL ('nil')` while the dump prints `v=null`. Two different things, both correct |
| everything else | the lexeme | symbol tokens carry their text, hence `got ASSIGN ('=')` |

Deviation list: **D-1** `peek` omitted (zero call sites). **D-2** the oracle's
inline membership tests (`current().type in ("EQEQ", "NEQ", "GT", "LT", "GTE",
"LTE")` and friends, plus `in EXPR_STARTERS`) become named helpers -
`is_compare_op(kind)`, `is_additive_op(kind)`, `is_multiplicative_op(kind)`,
`is_expr_starter(kind)`, `builtin_arity(kind)` - because MountX has no tuple or
set literal to test against; the members and their order are copied exactly.
**D-3** `eat` split into `eat` / `eat_is`. Nothing else is added, merged or
dropped: everything else in the port is shape, not structure.

## 10. Numbers as values, measured rather than hoped

A3 forces the port to convert, and phase 1's E3 said conversion is where ports
lie. So it was measured instead of argued. 24 decimal texts, deliberately
adversarial: the ones the repo contains (`0.5`, `1.0`); leading and trailing
zeros (`007.50`, `1.500000`, `3.1400`); the double-rounding boundaries
(`1.0000000000000002`, `9007199254740993.0`, `0.1234567890123456789`); tiny
values that print in exponent form (`0.00001`, `0.0000000001`); huge ones
(`99999999999999999999.5`, `10000000000000000.5`); and the `0.1` / `0.2` / `0.3`
family.

For every one of the 24, the MountX rendering of `string.tonum(text)` equals
`str(float(text))` in Python - which is the exact string the oracle's dump will
contain. `diff` was empty. Integers are exact and unbounded: `007` becomes `7`,
and a 20-digit run comes back digit-for-digit, because host integers are Python
ints and `tonum` accumulates with `intpart * 10 + d`.

The reason is structural rather than lucky. The host stores a MountX number as a
Python number and renders it with Python `str`, so *formatting* agrees by
construction, and `tonum`'s only float arithmetic is one correctly-rounded
division (`intpart + fracval / p`, `stdlib/text/string.mx:344-357`). Formatting
and division each round the same way on both sides; only the combination can
drift, and it did not drift on any of the 24.

Two things follow. Exponent-form **input** is impossible: `1e3` is refused by the
lexer (`Invalid number '1e'`), so `tonum` is never handed an exponent. But
exponent-form **output** is real and required - `0.00001` must print `1e-05`, and
it does, because that is what `str` does. A port that hand-rolled its own float
formatting to look tidy would fail here, which is why the dump rule (section 13)
says "coerce by concatenating with the empty string", never "format the float".

Residual risk: an unsampled text whose last bit disagrees. That is decision P3,
and its failure mode is a printed diff line containing one float, not a silence.

## 11. Recursion: the measurement that cancelled a design change

The belief going in was that a recursive-descent transcription would die on
moderately nested source: the expression ladder is nine frames deep per nesting
level, and the host stack looked like it gave out somewhere around a few hundred
levels. The proposed fix was to rewrite the ladder as an explicit-stack
precedence-climbing loop. **That fix is cancelled.** This is the data:

| probe | result |
|---|---|
| MountX, 9 mutually-recursive functions, one cycle = one nesting level | 50, 100, 150, 175, 200, 225 and 250 cycles all pass; **275 dies** with `[Runtime] maximum recursion depth exceeded` |
| frames at the boundary | 2,250 frames fine, 2,475 not - the host budget is roughly 2,300 MountX frames |
| oracle, nested parentheses | 1,000 levels parse, 2,000 raise `RecursionError`, under `sys.setrecursionlimit(10000)` (`main.py:9`) |
| deepest **paren nesting** in this repository | **2**, in `stdlib/text/string.mx` |
| deepest **block nesting** in this repository | **4**, same file |
| deepest **AST depth** in this repository | **16** |

Those last three rows measure different things and the port has two recursions, so
all three are needed: parentheses drive the parser's ladder (9 frames a level),
braces drive statement nesting (~4 a level), and AST depth drives the *dumper's*
walk (1 frame a level, section 13) - a deeply parenthesized expression produces no
AST depth at all, because `(e)` returns `e` unchanged. Worst file in the tree,
worst case: `2 x 9 + 4 x 4 = 34` frames against a budget of ~2,300.

The port therefore survives about 250 levels of the exact ladder shape while the
deepest parenthesization anywhere in the repository is 2. That is two orders of
magnitude of headroom, measured rather than estimated. Against it stands the cost
of the rewrite: an explicit-stack parser is a second parser. It would still have to
produce the same left-fold shape per precedence level, the same `BinOp.line`
(the operator token, section 6), and the same error position on a truncated
expression - all of which are free in recursive descent and tedious in a
hand-rolled state machine. Trading 68x headroom on the frame budget (34 frames
needed, ~2,300 available) for a second implementation of the grammar is not a
deal, so `parse_expression` transcribes as written.

What replaces it:

- a curated case at paren-depth ~120 - far above every file in the repo, safely
  below the measured cliff - so the ladder is genuinely exercised by the gate;
- a curated case with ~10 nested blocks, because statement nesting spends frames
  too (`parse_block -> parse_statement -> parse_if -> parse_block`, about 4 per
  level), and a 10-deep block nest with a mid-depth expression inside is the
  combined worst case worth pinning;
- the cliff recorded as a host limit rather than a parity case (**V1**, P2): a
  file that crosses it kills the port with `[Runtime] maximum recursion depth
  exceeded`, and the harness reports that as a crash with the runtime line
  attached, never as a pass.

## 12. Dispatch and cost model, stated before the fact

Three dispatch decisions, all settled by section 7's timings:

- **statement dispatch**: a 15-branch `if`-chain on `current().ttype` (the field
  name phase 1 chose for a token's kind), exactly as `parser.py:62-81` writes it.
  17 us per dispatch, measured.
- **membership tests** (`is_expr_starter`, `is_compare_op`, `is_additive_op`,
  `builtin_arity`): chains with an **early return**. The 20-test version costs
  24 us when every branch is evaluated and roughly half that when it can bail on
  the eighth. `builtin_arity(kind)` returns `-1` for a non-builtin and stands in
  for the oracle's `FILE_BUILTINS` dict; the four entries and their arities
  (`readx` 1, `writex` 2, `appendx` 2, `exportx` 2) are the whole table.
- **node construction**: `mk_*` builders. The dump's per-kind field table is data
  read linearly once per node; it is not a lookup structure and never becomes a
  `HashTable`, which at 4.5 ms a lookup would put the repo corpus at minutes for
  dispatch alone.

| activity | measured | projected onto the corpus (13,784 nodes) |
|---|---|---|
| build a node (instance + 3 field writes) | 12,000 in 97 ms -> ~8 us | ~0.11 s |
| build one dump line, stream it | 4,000 in 34 ms -> ~8.5 us | ~0.12 s |
| build the same dump as **one string** | 4,000 lines (199 KB) in 190 ms - 5.6x worse, quadratic in total length | ~1.1 s and growing; this is why `parse_print` exists |
| `xs = xs + [v]` | 4,000 appends in 36 ms | the largest node list in the repo is 152 entries |
| statement dispatch | 17 us | ~0.25 s |

All of that is small next to the interpreter stepping through the code, which is
what phase 1 found: the whole-repo lexer run is ~4.5 minutes and is dominated by
per-character dispatch, not by string or array building. The parse stage adds a
per-token and per-node cost of the same order, so M3 should be expected to land
in **minutes**, and the gate records the wall time instead of chasing it. If it
ever has to get faster the lever is the one phase 1 identified - fewer interpreter
steps per unit of work, e.g. hoisting the repeated `current()` reads into locals -
and none of it needs an interpreter change.

## 13. The canonical AST dump

One line per node. Fields separated by `|`. Pre-order: a node's line comes
before its children, and children follow in field order, then list order. Every
rule below was validated against a working oracle-side dumper before being
written down, so the format is known representable in MountX strings and known
able to carry all 35 shapes.

**Line shape.** `PATH|TYPE|line=N|field=...` where `line` is always the first
field. The field *order* is ours, not the oracle's dict order - Python dict
insertion order is not a semantic property of the AST and the dump must not
inherit it by accident. `PATH` is the root `/` extended per step: `/body[3]`,
`/body[3]/value`, `/body[3]/value/args[1]`. Paths carry the list indices, so a
diff points at a node rather than at a line number, and a missing child shifts
everything after it, which is the loudest possible failure.

**Field kinds.** A fixed vocabulary of field kinds, then a fixed field list per
node type - the schema table below uses these letters, with the oracle's
constructor line in brackets:

| kind | spelling in the dump |
|---|---|
| `s` string | `name=s` + `enc(text)` - the `s` prefix is what keeps `line=5` and a param named `5`-ish apart |
| `S` optional string | `parent=sBase`, or `parent=-` when the oracle has `None` |
| `n` node | `value=c`, and the child's own line follows |
| `N` optional node | `init=c` + child, or `init=-` |
| `L` node list | `body=3` - a count, then three child lines |
| `P` string list | `params=a,b` - joined, no count; empty is `params=` |
| `V` literal value | `v=` + the scalar rules below, always followed by `vt=INT` etc. |
| `-` no fields | `Break` and `Continue` and `This` and `Super` lines end after `line=` |

**The schema, per node type** (constructor line in brackets; `init` is a
`FuncDef`, `parent`/`var2`/`prompt`/`value` can be absent):

| node | fields, in dump order |
|---|---|
| `Program` [`:60`] | body(L) |
| `Let` [`:94`] | name(s) value(n) |
| `Print` [`:101`] | value(n) |
| `Import` [`:106`] | name(s) |
| `ClassDef` [`:141`] | name(s) parent(S) fields(L-FIELD) methods(L) init(N) |
| `FIELD` [`:129`] | name(s) value(n) |
| `FuncDef` [`:229`] | name(s) params(P) body(L) |
| `Return` [`:234`] | value(N) |
| `Break` [`:76`] / `Continue` [`:79`] | - |
| `TryCatch` [`:246`] | try_body(L) catch_var(s) catch_body(L) |
| `Throw` [`:251`] | value(n) |
| `If` [`:181`] | condition(n) then(L) else(L) |
| `While` [`:187`] | condition(n) body(L) |
| `Loop` [`:193`] | count(n) body(L) |
| `ForIn` [`:205`] | var(s) var2(S) iterable(n) body(L) |
| `Assign` [`:167`] | target(n) value(n) |
| `ExprStmt` [`:86`,`:168`] | expr(n) |
| `BinOp` [`:262`] | op(s) left(n) right(n) |
| `UnaryOp` [`:300`] | op(s) value(n) |
| `Call` [`:315`,`:360`] | callee(n) args(L) |
| `DotAccess` [`:319`] | object(n) field(s) |
| `Index` [`:324`] | array(n) index(n) |
| `Literal` [`:333`,`:336`] | value(V) value_type(T) |
| `Ident` [`:339`] | name(s) |
| `This` [`:342`] / `Super` [`:345`] | - |
| `Input` [`:354`] | prompt(N) |
| `TypeOf` [`:368`] | value(n) |
| `New` [`:384`] | class(s) args(L) |
| `ArrayLiteral` [`:419`] | elements(L) |
| `Read` `Write` `Append` `Export` [`:398`] | args(L) |

`If.else` is a list, not an optional node: `else { }` and `else if { }` both land
in it (`:177-180` puts a nested `If` in a one-element list), so `else=0` and
`else=1` are the only two shapes and an `else=-` never exists.

**Scalar rules for `V`.** `null` for None, `true`/`false` for BOOL, `s` + `enc(s)`
for strings, and for numbers **coerce, never format**: the value concatenated with
the empty string, which is the host's `str()` and therefore the oracle's `str()`
(section 10). `enc` is phase 1's function verbatim (`lexer.mx:246`, specified in
`DESIGN.md` section 13) - the same bytes on both sides, already tested, and the
reason `|` inside a string literal cannot forge a field.

**The 35 shapes.** 34 node types plus `FIELD`. A `ClassDef.fields` list holds
`FIELD` entries, which is the only place a list's element type differs from
"node", and it is the reason the walker takes the shape from the *parent's field
name* rather than from a key inside the child. `Program` is `body`; `If` is
`condition`, `then`, `else`; `ClassDef` is `name`, `parent`, `fields`, `methods`,
`init` (so an `init` never appears in `methods`, matching `:132-137`); `New` is
`class`, `args`; `TryCatch` is `try_body`, `catch_var`, `catch_body`; `Input` is
`prompt`; the four file builtins are `args` only.

**Spelling is not in the AST.** `lexer.py` gives several tokens more than one
spelling, and the parser records the *kind*, never the text: `length(a)`,
`lenx(a)` and `len(a)` all dump as `Call` over `Ident name=slen`; `a && b` and
`a and b` both dump `op=sAND`; `nil`, `null` and `nullx` both dump `v=null`;
`this`, `self`, `selfx` all dump `This`; `class`, `classx`, `new`, `newx`,
`throw`, `throwx`, `typeof`, `typex`, `typeofx` likewise. Three consequences: the
corpus must contain the alias spellings so this is proven and not assumed; the
dump must *not* try to carry the spelling; and the error templates that print
`{value!r}` must echo the **spelling actually written**, because there the lexeme
is the value (section 14).

**Sample.** This program - chosen because every rule in sections 6, 10 and 13 is
visible in one screen, and generated by the validated dumper rather than typed by
hand:

    mountx
    let a = [1, 2,]
    let b = len(a)
    let c = inputx
    let d = "x|y\tz"
    let e = -1
    let f = true
    let g = nil
    printx(a)
    class Box extends thing {
        let w = 0
        func init(v) {
            this.w = v
        }
    }
    let h = 1 +
        2
    callme
    (3)
    unmountx

dumps to this, complete and unabridged (the arrows annotate; they are not part of
the output):

    /|Program|line=1|body=11
    /body[0]|Let|line=2|name=sa|value=c
    /body[0]/value|ArrayLiteral|line=2|elements=2
    /body[0]/value/elements[0]|Literal|line=2|v=1|vt=INT
    /body[0]/value/elements[1]|Literal|line=2|v=2|vt=INT
    /body[1]|Let|line=3|name=sb|value=c
    /body[1]/value|Call|line=3|callee=c|args=1
    /body[1]/value/callee|Ident|line=3|name=slen           <- the len desugar
    /body[1]/value/args[0]|Ident|line=3|name=sa
    /body[2]|Let|line=4|name=sc|value=c
    /body[2]/value|Input|line=4|prompt=-                   <- bare inputx
    /body[3]|Let|line=5|name=sd|value=c
    /body[3]/value|Literal|line=5|v=sx\py\tz|vt=STRING     <- enc holds the pipe
    /body[4]|Let|line=6|name=se|value=c
    /body[4]/value|UnaryOp|line=6|op=sMINUS|value=c
    /body[4]/value/value|Literal|line=6|v=1|vt=INT         <- -1 is not a literal
    /body[5]|Let|line=7|name=sf|value=c
    /body[5]/value|Literal|line=7|v=true|vt=BOOL
    /body[6]|Let|line=8|name=sg|value=c
    /body[6]/value|Literal|line=8|v=null|vt=NULL
    /body[7]|Print|line=9|value=c
    /body[7]/value|Ident|line=9|name=sa
    /body[8]|ClassDef|line=10|name=sBox|parent=sthing|fields=1|methods=0|init=c
    /body[8]/fields[0]|FIELD|line=11|name=sw|value=c       <- no type in the oracle
    /body[8]/fields[0]/value|Literal|line=11|v=0|vt=INT
    /body[8]/init|FuncDef|line=12|name=sinit|params=v|body=1
    /body[8]/init/body[0]|Assign|line=13|target=c|value=c
    /body[8]/init/body[0]/target|DotAccess|line=13|object=c|field=sw
    /body[8]/init/body[0]/target/object|This|line=13
    /body[8]/init/body[0]/value|Ident|line=13|name=sv
    /body[9]|Let|line=16|name=sh|value=c
    /body[9]/value|BinOp|line=16|op=sPLUS|left=c|right=c   <- operator's line
    /body[9]/value/left|Literal|line=16|v=1|vt=INT
    /body[9]/value/right|Literal|line=17|v=2|vt=INT
    /body[10]|ExprStmt|line=19|expr=c
    /body[10]/expr|Call|line=19|callee=c|args=1            <- paren's line
    /body[10]/expr/callee|Ident|line=18|name=scallme
    /body[10]/expr/args[0]|Literal|line=19|v=3|vt=INT

`Assign.line` is 13 because it inherits the target `DotAccess`'s line (`:167`), and
the last three lines are the proof that `callme` newline `(3)` is one call spread
over two lines: callee on 18, `Call` on 19, `ExprStmt` inheriting 19. The full repo
dump is 22,227 lines over 38 files (1.50 MB - recorded by the P-M1 repo run, whose
manifest is what pins the count for a given checkout), which is what the harness
diffs. Two bugs this format caught in its own port, both invisible in a single-line
dump: `ClassDef.init` was emitted as `init=-` unconditionally, so every class with an
`init` lost that node, and `err_value` rendered `INT`/`FLOAT` from the lexeme and
`STRING` from the lexeme too, where the oracle has already converted the number and
decoded the string. `pcases/01`, `54`-`58` are the cases that say so.

## 14. Diagnostic catalog: sixteen templates, sixteen reached

`error(message)` (`parser.py:44-46`) prefixes `[Parser] Line {L}, col {C}: ` from
`current()` - the **offending** token, never the construct being parsed. That is
why `f().g.h = 1` reports the column of the `=` and not of the `f`. The port's
error path reads the same cursor, and must not be "helpfully" given the node's
line. E2 applies: MountX `throwx`es the identical message, one `ERR|<message>`
line in the dump, first error wins and everything after it is unreached.

### 14.1 The sixteen templates

| # | site | template | reached by | observed text |
|---|---|---|---|---|
| 1 | `:40` `eat_is` | `Expected {E}, got {T} ({v!r})` | `let = 1` | `Expected IDENT, got ASSIGN ('=')` |
| 2 | `:50` | `Program must start with mountx` | a file with no header; also `parse([])` | `Line 1, col 1: Program must start with mountx` |
| 3 | `:56` | `Program is missing unmountx` | `mountx let x = 1` + EOF | `Line 2, col 1: Program is missing unmountx` |
| 4 | `:59` | `Unexpected code after unmountx` | a second statement after the footer | `Line 4, col 1: Unexpected code after unmountx` |
| 5 | `:87` | `Unexpected token {T} ({v!r})` | `else { }` at statement position | `Unexpected token ELSE ('else')` |
| 6 | `:123` | `Missing closing }} for class '{n}' (opened on line {L})` | class left open to **EOF** | `Missing closing } for class 'A' (opened on line 1)` |
| 7 | `:134` | `Duplicate init function in class '{n}'` | two `func init` | `Duplicate init function in class 'A'` |
| 8 | `:139` | `Unexpected token {T} in class body (expected a 'let' field or 'func' method)` | `class A { printx(1) }` | `... PRINTX in class body (expected a 'let' field or 'func' method)` |
| 9 | `:158` | `Cannot assign to '{x}' - '{x} = ...' is invalid` | `this = 1` | `Cannot assign to 'this' - 'this = ...' is invalid` |
| 10 | `:160` | `Invalid assignment target: {T}` | `a + b = 1` | `Invalid assignment target: BinOp` |
| 11 | `:162` | `Cannot assign to a property of a temporary value` | `f().g.h = 1` | as template, at the `=` |
| 12 | `:164` | `Cannot assign into a temporary value` | `f()[0] = 1` | as template, at the `=` |
| 13 | `:213` | `Missing closing }} for block opened on line {L}` | `if true {` to **EOF** | `Missing closing } for block opened on line 1` |
| 14 | `:240` | `tryx must be followed by catchx(...)` | `tryx { }` with no catch | as template |
| 15 | `:392` | `{kw} expects {arity} argument(s), got {n}` | `writex("a")` | `writex expects 2 argument(s), got 1` |
| 16 | `:406` | `Unexpected {T} ({v!r}) inside expression` | `let x = { }` | `Unexpected LBRACE ('{') inside expression` |

All sixteen were reached against the oracle this session, and each row's text is
what the oracle printed - not a reading of the f-string. That audit matters in
both directions: phase 1 found a template that no input could produce, and here
two templates (6 and 13) turn out to need **EOF**, not merely a missing brace -
`class A { let x = 1` *followed by* `unmountx` produces template 8 instead, and
`if true { let x = 1 unmountx` produces 5. A corpus that only ever truncates at
`unmountx` would leave 6 and 13 untested, which is exactly the bug class this
catalog is for.

Two more measured surprises worth recording:

- **Template 10's value set is `{Call, BinOp}`, not "any type".** The path is
  entered only for `IDENT`/`THISX`/`SUPER` leads, `Ident`/`DotAccess`/`Index`
  pass to their own checks (11, 12), and `This`/`Super` are caught first (9). So
  `f() = 1` and `a + b = 1` reach it, while `(1) = 2`, `typeof(a) = 1` and
  `[1][0] = 1` never do - they fall out of the statement dispatch as a bare
  expression statement and die at template 5 on the leftover `=`. A port that
  reaches for a general "invalid target: {any}" message will look right and be
  untestable; the corpus pins both reachable values and one non-reachable shape.
- **Templates 6 and 13 interpolate an *opening* line into a message reported at
  the *closing* position** - `opened on line {open_tok.line}` while the prefix
  says `Line {current.line}`. Two different lines in one diagnostic, so the
  corpus case is a class or block opened near the top of a multi-line file.

### 14.2 Two renderers, three traps

The dump and the errors need *different* renderings of the same value, and only
one of them is phase 1's.

| | dump (`v=`) | error (`{v!r}`) |
|---|---|---|
| None | `null` | `None` - and reachable only via `EOF`, whose value is `None` |
| true / false | `true` / `false` | `True` / `False` - Python's bool repr, measured: `got BOOL (True)` |
| number | `"" + value` | same: `got INT (7)`, `got FLOAT (1.5)` |
| string | `s` + `enc(s)` | `py_repr(s)` - phase 1's function, quote-flip and all: `got STRING ("a'b")` |
| NULL token | `v=null` | `'nil'` - the **lexeme**, measured, because `lexer.py` never converts NULL |
| symbol | n/a | `py_repr(lexeme)`, so `'='`, `'else`, `'{'` |

So `err_value(tok)` is: EOF -> literal `"None"`; BOOL -> `"True"`/`"False"`;
INT/FLOAT -> `"" + tok_value(tok)`; STRING -> `py_repr(decoded)`; everything else
-> `py_repr(lexeme)`. It is a different function from the dump's scalar rule and
the two must not be merged, which is the single most likely place for a phase-2
implementation to go wrong while looking perfectly reasonable. A5 keeps the
U+00FF limit on `py_repr` from phase 1, and a string that leaves it fails loudly
in the curated corpus rather than silently.

Template 15 interpolates `{tok.value}` with **no** `!r` - the bare keyword
spelling - and all four file builtins have exactly one spelling each, so that one
needs no renderer at all.

## 15. What the corpus has to contain, and why

The repo corpus is 38 files and 22,227 nodes (re-counted at P-M1; the census below
was taken over the 33 files then in the tree, and every claim in it still holds), and
it still cannot certify most of what this document specifies: `Input` never appears,
six node types appear twice
or less, none of the 16 diagnostics fires, no line-provenance rule is
distinguishable on files whose constructs are single-line, and every alias
spelling in `lexer.py` happens to be written one way. Coverage claims come from
the curated set, so it is specified here rather than improvised later. Files live
in `selfhost/pcases/`, one corpus generator (`gen_pcases.py`), one dict shared by
both drivers - the phase-1 arrangement, unchanged.

| group | cases | what stops being assumed |
|---|---|---|
| **structure** | the section-13 program | the dump format itself, `enc` inside a node value, `-1` as `UnaryOp`, `[1, 2,]` trailing comma |
| **line provenance** | `1 +` newline `2`; `callme` newline `(3)`; a class field on its own line; a block whose `}` is 40 lines from its `{` | the seven rules of section 6 - all invisible in single-line code |
| **alias convergence** | `len(a)` / `lenx(a)` / `length(a)`; `a && b` / `a and b`; `nil` / `null` / `nullx`; `this` / `self` / `selfx`; `class` / `classx`; `new` / `newx`; `throw` / `throwx`; `typeof` / `typex` | section 13's claim that spelling is absent from the AST, which is a property to test, not to assume |
| **rare shapes** | bare `inputx`, `inputx()`, `inputx(p)`; `loop 3`; `readx(p)`; `writex(p, c)`; `appendx(p, c)`; `exportx(p, v)`; `super` in a method; `continue`; `break`; `throw e`; `tryx/catchx`; `for k, v in m`; `newx dict.Dict("a")`; `class D extends dict.Dict` | the eight node types that appear five times or less in 13,784 nodes (section 5), plus the two dotted-name paths |
| **degenerate** | `mountx unmountx` (empty `Program`); `[]`; `f()`; `class E { }` (`fields=0 methods=0 init=-`); `func g() { return }` (`value=-`); `let x = ((((1))))` | every `=-` and `=0` in the format, which are exactly where a port that pre-fills fields hides |
| **depth** | paren nesting at ~120; a ~10-deep block nest with an expression inside | section 11: the ladder runs, and the margin is real, not hoped |
| **diagnostics** | all 16 rows of section 14, the two EOF variants *without* `unmountx`, the two truncated-at-`unmountx` variants that produce different messages, `(1) = 2` and `typeof(a) = 1` as the shapes that must NOT produce template 10 | every template reached, and the negative cases that pin the set |
| **renderers** | `let true = 1`, `let nil = 1`, `let 007 = 1`, `let 1.5 = 1`, `let "ab" = 1`, `let "a'b" = 1` (quote-flip), plus one EOF case (`(None)`) | the `err_value` table, including `True` and `'nil'`, which no amount of reading the f-string would earn |

Two corpus rules inherited from phase 1 and worth restating because they are
cheap to violate: sources are written so that a `#` comment never sits at the end
of a line that also matters (comments are discarded by the lexer, so a case that
depended on one would be testing nothing), and the file-based cases cannot carry
CRLF because both sides normalize it - if a CRLF case is ever wanted it goes in
the inline set, spelled identically in both drivers.

## 16. The parity harness

    selfhost/gen_pcases.py     writes pcases/ from one dict
    selfhost/pcases/*.txt      one corpus case per file
    selfhost/parser.mx         the port
    selfhost/parse_parity.mx   MountX driver: curated + inline cases, via lexer.mx
    selfhost/parse_repo.mx     MountX driver: the repo corpus, via lexer.mx
    selfhost/parity_parse.py   oracle driver: same corpus through parser.parse

    python3 selfhost/parity_parse.py --check          # curated + inline
    python3 selfhost/parity_parse.py --repo --check   # every .mx in the repo

Three decisions the harness has to get right, all learned in phase 1:

- **Both modes are compared.** A case that parses on both sides is compared as a
  dump; a case that errors on both sides is compared as one `ERR|<message>` line;
  a case that parses on one side and errors on the other is a failure in itself,
  which is the interesting case and must not be filtered out.
  **F1, corrected at P-M1** (this was mislabelled V1 here at delivery; V1 is the
  recursion divergence of section 18). This row previously claimed `fbtest.mx` "contains an
  inner `mountx`" so that the oracle answers `Unexpected token MOUNTX ('mountx')` at
  line 5. It does not: fbtest.mx has exactly one `mountx`, on line 4, and the oracle
  parses the whole file (`body=416` nodes). It is therefore a successful-parse case
  in the repo stage, which is still worth having - it is the largest pure-parse file
  in the tree - but the inner-`mountx` construct it was supposed to cover needed its
  own case, and has one: `pcases/61-inner-mountx.txt` puts a second `mountx` on line
  3 and answers `[Parser] Line 3, col 1: Unexpected token MOUNTX ('mountx')`, the
  message this section described, at a line the corpus can show.
- **The repo stage is `.mx` only.** `parity.py`'s phase-1 walk takes `.mx` and
  `.py`, which is right for a lexer and wrong for a parser: a `.py` file is not
  MountX source, and every one of them would produce the same
  `Program must start with mountx` at different columns - six near-identical
  lines of noise. The `.py` files stay in the *lexer* gate, where they are real
  coverage. The port still cannot walk a tree, so the manifest is written by the
  oracle driver and read back, same as phase 1, filtered to `.mx` and recorded in
  the manifest so both sides are reading one list.
- **The wrapper is part of the input, on both sides** (A4). `parity_parse.py`
  wraps a headerless case before lexing it and prints the wrapper in the case
  file, so the corpus text always shows exactly what was parsed and `line` fields
  in the dump cannot silently disagree with the source shown beside them.

## 17. Milestones

| stage | deliverable | exit criterion | status |
|---|---|---|---|
| P-M0 | this document | you approve it, or hand back edits to P1-P8 | delivered |
| P-M1 | `parser.mx` + `pcases/` + both drivers | empty diff over the curated corpus, **and** the phase-1 gate (`parity.py --check`) still green, **and** all 16 diagnostics reached | **delivered** - curated: 49 cases, 496 dump lines, 0 differences, 16/16 templates; repo: 38 `.mx`, 22,265 lines, 0 differences, 414.5 s; phase 1: `PARITY OK: 52 cases` |
| P-M2 | the port parses `selfhost/lexer.mx` | node-for-node match on a real ~1,300-line file - classes, fields, `super`, `tryx`, `loop`, the lot | **met inside the repo stage** - `lexer.mx` is in the manifest and its 1,300-line dump matched; not gated on its own, so treat it as unreviewed until you say otherwise |
| P-M3 | whole repo | 38 dumps, empty diff, wall time recorded (minutes expected, section 12) | **delivered early** - the P-M1 repo stage (`parity_parse.py --repo --check`) already walks every `.mx`, and `fbtest.mx` is a clean parse rather than the error case section 16 claimed (F1) |
| P-M4 | hardening | every template reached *and* every `err_value` row exercised, not just the sampled ones | **met inside the repo stage** - 16/16 templates from 16 distinct cases, and all seven rows of 14.2 have their own case: `50`/`51` BOOL, `54` INT, `55` FLOAT, `56`-`58` STRING (tab, quote-flip, backslash), `52`/`53` NULL in two spellings, `59` EOF, `60` a symbol |
| P-M5 | the port parses `selfhost/parser.mx` - its own source | empty diff. The phase-1 self-lexing trick, but on a parser, which is the honest read of "MountX can describe itself" | **met inside the repo stage** - `parser.mx` is in the manifest and its own dump matched; same caveat as P-M2 |
| P-M6 | *(optional, needs separate approval)* the port behind a flag in `main.py` | an interpreter change; explicitly **not** phase 2 | out of scope |

P-M2 before P-M3 on purpose: `lexer.mx` is the largest file in the tree that this
document's author can predict the shape of, and a bug that shows up there is a
bug that will show up in the repo run anyway.

## 18. Decisions register - your call, each one changes the code

| # | issue | recommendation | alternative |
|---|---|---|---|
| P1 | node representation | one `Node` class, `kind` + sparse fields, 35 `mk_*` builders (section 8) | 34 subclasses; same builders, plus a name-to-class table nobody needs |
| P2 | precedence ladder | transcribe the recursion; the ladder survives 250 levels, the deepest parenthesization in the repo is 2 (section 11) | explicit-stack rewrite, which is a second parser to keep in sync |
| P3 | number equality in the AST | value equality, coerced with `""` - 24 of 24 adversarial texts agree today | compare `value_type` only for `FLOAT` and keep a separate value-parity test, if P-M3 ever shows a drift |
| P4 | where token conversion lives | `tok_value(tok)` in `parser.mx` | a `Token.value_converted()` method, which blurs E3 and hands phase 3 a footgun |
| P5 | token spelling in the dump | absent, as in the oracle; alias cases pin it | carry the lexeme on every node - a field the oracle does not have, so it would need inventing |
| P6 | class fields | dump as `FIELD`, two fields, modelled as a pair | synthesise a `Let` type for them, which the oracle never emits |
| P7 | dead code | omit `peek` (zero call sites); keep `current`'s past-end EOF (public `parse` can be handed a short array) | port both verbatim, or drop both and risk an index error where the oracle has a diagnostic |
| P8 | repo stage scope | `.mx` only | keep `.py`, which adds six identical `Program must start with mountx` lines |

**V1, the one accepted divergence.** The port's expression ladder gives out
around 250 levels of nesting; the oracle reaches roughly 1,000 under
`sys.setrecursionlimit(10000)` (`main.py:9`); the deepest parenthesization
anywhere in this tree is **2** levels, and the deepest block nest 4 (section 11).
The divergence is loud (`maximum recursion depth exceeded`, a crash the harness
reports as one) and is a property of the host, not of the parse. The alternative
is raising the host's limit for the port run, which is a `main.py` change and
needs its own approval.

## 19. Open questions

1. P2 / V1 - accept the recursion cliff with the corpus depth case, or lift
   `setrecursionlimit` for parity runs (a `main.py` change, small but real)?
2. Should phase 2's parity gate join `fbtest.mx` - phase 1's D9, still unanswered,
   and phase 2 inherits whatever that answer is.
3. P8 - do you want the `.py` files kept in the parse run as deliberately-failing
   negative cases rather than dropped?
4. Are `pcases/` checked in (phase-1 style, one file per case) or regenerated on
   demand? Checked-in wins on reviewability - a golden corpus you cannot diff
   against last week is not a golden corpus.
5. After P-M5 - does phase 3 (the interpreter) get designed against
   `interpreter.py` as the next oracle, or against a smaller slice of it? The AST
   shape in this document is the input to that question, and section 5a is already
   a hint that the interpreter's own dict assumptions need the same audit.

## 20. What this phase will not touch

`parser.py`, `lexer.py`, `main.py`, `interpreter.py`, `fbtest.mx`, `stdlib/`, and
`selfhost/lexer.mx` - the last one most pointedly, since section 9 goes out of its
way to keep `Token` untouched so phase 1's gate does not move. New files only:
`selfhost/parser.mx`, `selfhost/parse_parity.mx`, `selfhost/parse_repo.mx`,
`selfhost/parity_parse.py`, `selfhost/gen_pcases.py`, `selfhost/pcases/*`. Two
docs gain a paragraph at delivery (`README.md`, `AGENTS.md`: the parse-parity
command joins the test protocol with the rule "any change to `parser.py` re-runs
the gate"), and **no VERSION bump** - nothing a user can observe changes.

## 21. Epilogue - phase 2 as delivered

Everything above was written before the port existed. This is what happened, and
the only part of this document that did not survive contact is F1 below.

### 21.1 Final numbers

| measure | value |
|---|---|
| port | `selfhost/parser.mx`, **1,386 lines**, plus `parse_parity.mx` (82) and `parse_repo.mx` (60) as drivers |
| curated corpus | **49 cases** in `pcases/`, written by `gen_pcases.py` from one dict; **496 dump lines**; regenerates byte-identically |
| repo corpus | **38 `.mx` files**, **22,265 dump lines**, **22,227 nodes**, **1.50 MB** of dump; manifest `parse_repo_files.txt` rebuilt each run |
| diagnostics | **16/16 templates reached**, each from its own case, each verified as the template it claims |
| curated gate | `PARITY OK (curated): 49 cases, 496 dump lines, port 7.4s, 16/16 templates reached` |
| repo gate | `PARITY OK (repo): 38 cases, 22265 dump lines, port 387.03s` (twice: 414.5 s, 387.0 s) |
| port speed | **~57 nodes/s** per layer (22,227 nodes / 387 s) - the number phase 3 designs against |
| phase-1 gate | still green: `PARITY OK: 52 cases, 255 lines` curated, and green over the repo |

### 21.2 What the corpus caught, and why each one was silent

The corpus paid for itself before its first green run, which is the argument for
building a corpus rather than reading the grammar aloud in MountX. Three defects,
all in `parser.mx`, none of them visible in a single-line dump:

1. **`ClassDef` emitted `init=-` unconditionally.** A class *with* an `init` lost
   the whole `FuncDef` subtree - every method printed, only the init vanished. Silent
   because `init=-` is a legal rendering for a class with no init, so the output was
   well-formed and merely wrong, for most of the stdlib. The golden sample in section
   13 carries `|init=c`, and that one character is what caught it.
2. **`err_value` echoed the lexeme for `INT` and `FLOAT`.** `lexer.py:169-172`
   converts the number before the parser sees it, so `let 007 = 1` has to read
   `got INT (7)` and `let 1.500 = 1` has to read `got FLOAT (1.5)`. The port printed
   `007` and `1.500`. Silent because it is only observable on the error path, and
   only for a literal whose spelling and value differ.
3. **The `NULL` hardcode, in the copy that matters.** `err_value` returned `"'nil'"`
   for every NULL token, so `let nullx = 1` reported a spelling the program does not
   contain - the same defect C1 flagged in `tok_value`, in the one function whose
   output the dump can see. `STRING` next to it rendered the lexeme where the oracle
   has the *decoded* value, which is wrong for `"a'b"` and for `'a\tb'`. Now
   EOF renders `None`, BOOL renders `True`/`False`, INT/FLOAT render the converted
   value, STRING renders the decoded string, and everything else renders the lexeme -
   section 14.2 as written. The code had drifted from the document, not the other
   way round.

### 21.3 Milestone reconciliation

The repo stage swallowed four milestones, because a stage that walks every `.mx` in
the tree necessarily walks `lexer.mx`, necessarily walks `parser.mx`, and necessarily
reaches every template once the curated corpus does. Recorded rather than hidden:

| stage | what actually happened |
|---|---|
| P-M1 | delivered as specified - both gates green, all 16 templates, phase 1 unmoved |
| P-M2 | **retired as a named milestone.** `lexer.mx` is in the repo manifest and its ~1,300-line dump matched, so the criterion is met; what P-M2 was *for* - a large real file as the first non-trivial input - arrived one stage earlier than planned. There is nothing left to schedule |
| P-M3 | delivered inside P-M1: 38 dumps, empty diff, 387 s, against section 12's "minutes expected" |
| P-M4 | delivered inside P-M1: 16/16 templates from 16 distinct cases, and all seven rows of 14.2 have their own case (`50`-`60`) rather than being sampled |
| P-M5 | delivered inside P-M1: `parser.mx` is in the manifest, so the port parsed its own 1,386-line source and the diff was empty |
| P-M6 | not started, as planned - it is an interpreter change, and needs its own approval |

The honest caveat on the four rows above: each was satisfied *as a side effect of a
gate built for something else*, which is not the same as having been reviewed on its
own terms. P-M5 is the one worth a second look if you want one, because "the parser
parses itself" is the claim a reader will take most literally: what was proven is
that `parser.mx`'s **source text** round-trips through the port with an identical
tree. It is not a claim about the port parsing the *tokens it produces* (that was
phase 1's version of the trick, and remains phase 1's).

### 21.4 F1 - the fbtest.mx header claim, resolved

Section 16 asserted that `fbtest.mx` contains an inner `mountx` and therefore answers
`Unexpected token MOUNTX ('mountx')` at line 5, which made it a free error case. It
does not: `fbtest.mx` has exactly one `mountx`, on line 4, and the oracle parses the
whole file into a `Program` with 416 statements. Two consequences, both taken:

- `fbtest.mx` is in the repo stage as a **successful parse**, which is still the most
  valuable thing it can be here - it is the largest pure-parse file in the tree, and
  the largest file the two parsers must agree on statement for statement.
- The construct that row was invented to cover had no case at all, so it has one:
  `pcases/61-inner-mountx.txt`, a second `mountx` on line 3, answering
  `[Parser] Line 3, col 1: Unexpected token MOUNTX ('mountx')` - the message and the
  line the design predicted, now in a file that shows it rather than a sentence that
  claims it.

### 21.5 V1 - the recursion cliff, disposition

Accepted as a host limit. No `setrecursionlimit` change, no `main.py` change, no
precedence-climbing rewrite. The corpus pins the shape well below the cliff (120
parens, 10 nested blocks, both matching; the cliff is at ~275 levels), and the
harness treats a `RecursionError` as a loud crash rather than a pass. The 2,300-frame
host budget from section 11 is now a phase-3 input as well - see
`INTERP_DESIGN.md`, which has to spend that same budget through a second layer.

### 21.6 The wrapping asymmetry, which was the right call

Two wrapper rules in one harness looks like a bug and is the design:

- The **curated stage never wraps.** A case must be able to have no header, or
  template 2 (`Program must start with mountx`) is unreachable by construction and
  the whole error surface of `parse()` goes untested.
- The **repo stage wraps by `main.py:30-33`'s predicate**, which asks whether the
  *first token* is `MOUNTX` - not whether the text starts with the word. 28 of the 38
  files are headerless and several carry a comment before any header, so the naive
  text test would have produced 28 bogus `Program must start with mountx` lines
  instead of 38 dumps. Porting the predicate exactly is what made the repo stage
  meaningful on the first try.

### 21.7 What phase 2 changed outside `selfhost/`

Section 20 promised `README.md` and `AGENTS.md` would each gain a paragraph. Half of
that was taken:

- **`AGENTS.md`: yes.** The two parse-parity commands joined the test protocol, with
  the trigger rule (any change to `lexer.py`, `parser.py`, or `selfhost/*.mx` re-runs
  all four gates) and the divergence list (`D3`, `D5`, `V1`). That is the phase's real
  payoff: `parser.py` had no differential test at all before this, and now has 49
  curated cases and every `.mx` file in the tree.
- **`README.md`: no - and it could not have.** The file does not exist in this
  checkout: `find . -iname '*readme*'` returns nothing, while `AGENTS.md` refers to it
  five times ("README.md is the user guide", the checklist line "README.md updated",
  the closing handoff paragraph). Nothing was owed here anyway - a reader-facing
  document should not gain a paragraph about a compiler component that no
  command-line flag can reach, and P-M6 is where such an entry belongs - but the
  missing file is a hole in the handoff kit on its own merits. A rule that points at a
  file nobody can open is not enforced by anyone, and this phase's checklist item
  three is currently unactionable.
- **No VERSION bump**, as promised - nothing a user can observe changed. `fbtest.mx`
  is untouched, so its header count did not move.

### 21.8 What phase 3 inherits

Four things, each of which is a constraint rather than a convenience:

1. **~57 nodes/s per layer.** The parse stage costs about 390 s over the repo. An
   interpreter layer over the same 22,227 nodes is not free, and `fbtest.mx` as the
   differential anchor runs 277 assertions through *three* layers if the port drives
   it. Budget the wall time before designing the corpus, not after.
2. **One host stack, two spenders.** The ~2,300-frame budget of section 11 is
   consumed by the port's own recursion *and* by every guest call the interpreter
   makes through it. Phase 3 has to measure its own frames-per-guest-level against
   what is left, in the same way section 11 measured the ladder.
3. **The three-command pattern works, and is now the house shape.** A generator, a
   MountX driver, an oracle driver, a golden corpus checked in as files, and a repo
   stage that walks the tree. Phase 3 should not invent a fourth shape.
4. **A corpus finds what a reading of the grammar cannot.** Three defects in
   `parser.mx`, all invisible in isolation, all found by diffing against something
   that already worked. The interpreter port is a much larger body of logic than the
   parser port was - natives, the environment chain, `values_equal`, `super` - and
   the odds that a transcription of it is right first time are correspondingly
   smaller. The differential harness is the deliverable, not the `.mx` file.

