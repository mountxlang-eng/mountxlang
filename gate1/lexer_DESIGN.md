<!-- Copyright (C) 2026-2027 Ziad Mohsin. All Rights Reserved. -->
# selfhost/lexer.mx - design (phase 1: lexer only)

Status: **proposal. No code exists.** Everything below is a contract to
approve before a line of MountX is written. Decisions that need your call are
numbered `D1..D9` and collected in section 17.

---

## 1. Goal

Re-express `lexer.py` in MountX as `selfhost/lexer.mx`, such that for any
input the MountX lexer yields the same token stream and the same diagnostic
text as `lexer.py`. Prove it differentially, with `lexer.py` as the oracle.

Why it is worth doing: it is the only honest proof that MountX is expressive
enough to describe itself, and it turns the lexer into a *specified* thing -
the parity harness (section 14) also becomes a real regression net for
`lexer.py` itself, which today has no tests beyond `fbtest.mx` happy paths.

## 2. Non-goals

- No parser, no AST, no interpreter. Phase 2/3 concerns - phase 2 is specified
  in `selfhost/PARSER_DESIGN.md`, written against the port and harness this
  document delivered.
- No change to `lexer.py`, `parser.py`, `main.py` or `interpreter.py` -
  including **no new native builtins** (section 15 explains how to live
  without the one we want).
- Not a performance play. `lexer.py` stays the production lexer; the MountX
  one is a capability proof and an oracle fixture.
- Not importable from the repo root: `importx lexer` searches
  `$MOUNTX_STD -> script_dir -> cwd -> interpreter dir`, each for
  `stdlib/<cat>/`, `stdlib/`, `libs/` and the base itself. `selfhost/` is not
  among them, and adding it would be an interpreter change. Consequence: the
  driver script must live *inside* `selfhost/` so that `script_dir` resolves
  `selfhost/lexer.mx` as a sibling.

## 3. The parity contract

Four rules. If you disagree with one, say so now - they decide the shape of
everything else.

**E1 - token stream equality.** A run emits one canonical line per token, in
order, ending with an `EOF` line (section 13). Two implementations are equal
when their emitted lines are byte-identical.

**E2 - diagnostic equality is at the message level, not the process level.**
`lexer.py` raises `SyntaxError`, and `main.py:format_error` prints a
`SyntaxError` verbatim when the text already starts with `[` - that is why
`12abc` prints exactly:

    [Lexer] Line 2, col 9: Invalid number '12abc' - a number cannot be
    followed by letters. Write '12' and 'abc' separately

MountX cannot raise `SyntaxError`. `throwx "<msg>"` prints
`[Runtime] Uncaught throwx: <msg>`, and `catchx` hands back the raw string.
So the MountX side throws the **exact message string** and the harness
compares that string to the oracle's. We do not try to match stderr.

**E3 - numbers are compared as lexemes, not values.** `lexer.py` calls
`int(text)` / `float(text)`, i.e. correctly-rounded parsing. The only
MountX-side parser is `string.tonum` (stdlib/text/string.mx), which is
 decimal and builds fractions arithmetically (`intpart + fracval / p`) -
verified: `"007"` -> `7`, `"1.50"` -> `1.5`, `"ff"` -> `null`. Arithmetic
construction can disagree with `float(text)` in the last bit for some inputs,
so canonical output carries the **lexeme** (`1.50` stays `1.50`). A separate,
small value-parity test covers the integer and one-decimal-place cases where
both agree exactly.

**E4 - explicitly out of scope for equality:** Python `repr` of floats,
process exit codes, `printx` rendering, and any diagnostic whose text comes
from `decode_string` (section 12 explains why those are unreachable).

## 4. The oracle, in one page

`lexer.py` is a single ordered alternation (`TOKEN_SPEC`, lines 3-72, 66
entries) compiled into one regex and driven by `finditer`. Table order **is**
priority order. The MountX port keeps the same ordering, in the same groups.

### 4.1 Keyword-led (entries 1-34, then 38)

All are word-bounded (`\b...\b`) except the three word-operators below.
`MOUNTX` and `UNMOUNTX` are ordinary keywords here - the lexer does not
enforce that a program is wrapped; `main.py` auto-wraps and the parser
complains (`[Parser] Line 3, col 1: Program is missing unmountx`).

| # | token | spellings (verbatim) |
|---|-------|----------------------|
| 1 | MOUNTX | `mountx` |
| 2 | UNMOUNTX | `unmountx` |
| 3 | PRINTX | `printx` |
| 4 | INPUTX | `inputx` |
| 5 | IMPORTX | `importx` |
| 6 | READX | `readx` |
| 7 | WRITEX | `writex` |
| 8 | APPENDX | `appendx` |
| 9 | EXPORTX | `exportx` |
| 10 | TRYX | `tryx` |
| 11 | CATCHX | `catchx` |
| 12 | THROWX | `throwx` `throw` |
| 13 | TYPEOF | `typeofx` `typeof` `typex` |
| 14 | NULL | `nullx` `null` `nil` |
| 15 | CLASSX | `classx` `class` |
| 16 | NEWX | `newx` `new` |
| 17 | THISX | `thisx` `this` `selfx` `self` |
| 18 | SUPER | `super` |
| 19 | EXTENDS | `extends` |
| 20 | LET | `let` |
| 21 | FUNC | `func` |
| 22 | RETURN | `return` |
| 23 | IF | `if` |
| 24 | ELSE | `else` |
| 25 | LOOP | `loop` |
| 26 | WHILE | `while` |
| 27 | FOR | `for` |
| 28 | IN | `in` |
| 29 | BREAK | `break` |
| 30 | CONTINUE | `continue` |
| 31 | LEN | `lenx` `len` `length` |
| 38 | BOOL | `true` `false` |

48 spellings, 40 token names, matching is **case-sensitive** (`MountX` is an
`IDENT`, not a keyword).

### 4.2 Word-operators - the tricky three (entries 35-37)

Each pairs a symbol alternative with a word alternative, and each has a
boundary or lookahead condition:

    AND   &&   or  \band\b
    OR    ||   or  \bor\b
    NOT   !    (only when NOT followed by =),  or  \bnot\b

`NOT` is the one that must be copied deliberately: `!` is `NOT`, `!=` is
`NEQ`. In the port the two-char table is consulted first, which reproduces
the negative lookahead exactly. Nothing to decide - just do not forget it.

### 4.3 Number-led (entries 39-42) - order is load-bearing

    39 BAD_NUM_SUFFIX      \d+(?:\.\d*)?[a-zA-Z_]\w*
    40 FLOAT               \d+\.\d+
    41 BAD_FLOAT           \d+\.(?![0-9])
    42 INT                 \d+

`lexer.py`'s own comment says it: BAD_NUM_SUFFIX must precede FLOAT so `1.5y`
is rejected rather than lexed as `1.5` + `y`. See section 9 for the full
state machine and the `num`/`rest` message nuance.

### 4.4 String-led (entries 43-44)

    43 STRING              "(?:\\[\\"/bfnrt]|\\u[0-9a-fA-F]{4}|[^"\\\n])*"
    44 UNTERMINATED_STRING "(?:\\.|[^"\\\n])*

Both stop at a newline: **no token can span a line**. See section 10.

### 4.5 Symbol-led (entries 45-62), exactly this order

| # | token | text | # | token | text |
|---|-------|------|---|-------|------|
| 45 | EQEQ | `==` | 54 | DIV | `/` |
| 46 | NEQ | `!=` | 55 | MOD | `%` |
| 47 | GTE | `>=` | 56 | LPAREN | `(` |
| 48 | LTE | `<=` | 57 | RPAREN | `)` |
| 49 | GT | `>` | 58 | LBRACE | `{` |
| 50 | LT | `<` | 59 | RBRACE | `}` |
| 51 | ASSIGN | `=` | 60 | LBRACK | `[` |
| 52 | PLUS | `+` | 61 | RBRACK | `]` |
| 53 | MUL | `*` | 62 | COLON / DOT / COMMA | `:` `.` `,` |

Two facts to write down before anyone "improves" them:

- The complete set of multi-character operators is `== != >= <= && ||`.
  Nothing else in the language consumes two characters.
- These operators do **not** exist, and a stray second character is a
  `MISMATCH`, not a compound: `+= -= ** // -> => <> !== === ~ & | ++ --
  ?:`. Enumerated here so the port does not invent them.

### 4.6 Filler, identifiers, fallback (entries 63-66)

    63 COMMENT   #[^\n]*      discarded, stops before the newline
    64 SKIP      [ \t\r\n]+      discarded
    65 IDENT     [a-zA-Z_]\w*
    66 MISMATCH  .            any remaining non-newline char -> error

## 5. Matching semantics the port must reproduce

**First hit in table order wins - this is not longest-match.** At each
current position the alternatives are tried top to bottom and the first that
matches consumes. Longest-match would silently change `1.5y` and `==`.

**The scan is total.** `SKIP` eats every newline and `MISMATCH` matches every
other leftover character, so `finditer` never silently advances past an
unmatched character. The port must therefore have no "skip a char and try
again" path: every position is either consumed by a rule or becomes an error.

**Position is the start of the match**, and because no token spans a newline,
a token's line and column are fully determined by where it started (section
11).

## 6. Derived dispatch, and why it keeps the order

Reading 66 alternatives per character would be slow and ugly. Grouping them by
lead character is safe **only because the groups are disjoint by lead
character**. The port may use this dispatch, and the parity harness is what
makes that claim checkable rather than hopeful:

| lead character | tables tried, in this order |
|---|---|
| letter or `_` | word: keyword spellings (4.1), then `IDENT` (65) |
| digit | number (4.3): BAD_NUM_SUFFIX, FLOAT, BAD_FLOAT, INT |
| `"` | string (4.4): STRING, then UNTERMINATED_STRING |
| `#` | COMMENT (discarded) |
| space, tab, CR, LF | SKIP run (discarded) |
| `= ! > < & \| + - * / % ( ) { } [ ] : . ,` | two-char table, then one-char table (4.2 symbols + 4.5) |
| anything else | MISMATCH -> error (4.6) |

The word path is where `\b` goes. Python matches `\blet\b` and fails on
`letter`; the port instead consumes the **maximal word run** first and then
looks the whole word up. That is equivalent for every input, because a
word-bounded keyword can only fire when the maximal run equals the spelling.
Worked check-list the harness must contain: `letter`->IDENT, `nilx`->IDENT,
`truex`->IDENT, `and_or`->IDENT, `notx`->IDENT, `nil`->NULL, `selfx`->THISX,
`throw`->THROWX, `MountX`->IDENT.

The `and` / `or` / `not` words live in the word table; their `&&` `||` `!`
siblings live in the symbol table. No cross-talk, one lookup per path.

## 7. Host constraints (each one measured, none assumed)

The design is shaped by what the host actually does. Every row below was run
against this working tree, not recalled from memory.

| fact | observed |
|---|---|
| string indexing | `"abc"[1]` -> `b`, type `string`; 0-based, **code-point** granularity (`"h\u00e9llo"[1]` -> `\u00e9`, `len` -> 5) |
| negative / float index | `s[-1]` -> `[Runtime] Index -1 out of range (length 5)`; non-integer index rejected - so no Python-style `s[-1]`, always guard `pos > 0` |
| array growth | `a[1] = 9` works **in range**; `a[3] = 9` on a length-1 array -> `[Runtime] Index 3 out of range (length 1)`. Arrays grow only via `xs = xs + [v]` |
| char <-> code natives | **none exist** (`char_code` / `char_from` / `ord` / `chr` are not in `interpreter.py`) - see section 15 |
| char -> small integer | `string.index_of("0123456789", "7")` -> `7`; returns `-1` when absent. This is the workaround primitive |
| number text -> value | `string.tonum` is decimal-only: `"007"` -> `7`, `"1.50"` -> `1.5`, `"ff"` -> `null`; trims input, so it must only ever be handed an exact digit run |
| `string.substr(s, a, b)` | end-**exclusive**, clamps both ends, and is itself `out = out + s[i]` (quadratic) - fine for short lexemes, not for whole files |
| `string.join(arr, sep)` | exists; this is how char arrays become strings |
| closures | none. A function sees its own scope, module scope, and globals - nothing captured. Callbacks are impossible (see `emit` in section 8) |
| module top level | runs at import time **in statement order**; a module-level `let` initializer cannot call a function defined later in the same module (`Undefined variable`), so builders must be declared above the `let`. Module functions may reassign module lets, so lazy init works |
| reading a module let from outside | returns the **import-time snapshot**, not the live value - accessors must be functions, never `lexer._KW` |
| regex | none, obviously; the whole point |
| string building cost | quadratic, measured: 800 appends in about 1 ms. Cheap per lexeme, expensive per token array (section 16) |
| error text routing | a `SyntaxError` whose text starts with `[` prints verbatim; `throwx "[Lexer] ..."` prints `[Runtime] Uncaught throwx: [Lexer] ...`, and `catchx` hands back the raw string (`display` does not quote strings) |

## 8. Shape of the module

One file, `selfhost/lexer.mx`, wrapped in `mountx ... unmountx`, importing
only `string` and `hashtable`. No `io` (the driver reads the files), no
`error` (we throw plain strings, per E2).

**Data model - two classes, no bare arrays.** Objects are the only mutable
state MountX offers a function, which is what replaces closures:

- `Token` fields: `ttype` (name, e.g. `BAD_FLOAT`), `lexeme` (exact source
  text, before value conversion), `line`, `column`. `init` stores all four;
  `canonical()` renders the section 13 line. The field is `ttype` rather than
  `type` because `type` is a stdlib module name and `this.type` invites
  confusion; `lexeme` is kept because E3 needs it and because it is what the
  `BAD_*` diagnostics quote.
- `Lexer` fields: `src`, `n`, `pos`, `line`, `column`, `tokens`.
  `pos`/`line`/`column` advance as characters are consumed instead of being
  recomputed per token - same result, see section 11.

**Public surface** (contract, not code):

| entry | returns | raises |
|---|---|---|
| `lex(src)` | array of `Token`, always terminated by an `EOF` token | section 12 messages, as `throwx` values |
| `tokenize(src)` | alias for `lex`, mirroring `lexer.py:tokenize` | same |
| `lex_print(src)` | nothing; prints one canonical line per token and stops at the first error | same |
| `decode_string(raw)` | the decoded value of a **closed** literal, including its quotes | the three section-12.3 messages |
| `classify(word)` | token name for a whole word, or `IDENT` | never |
| `render(tokens)` | the canonical lines joined by newlines | never |

`lex_print` exists only because callbacks do not. The alternative - returning
a giant array from a corpus run - pays the quadratic token-append cost of
section 16 for no benefit. `lex` still exists for phase 2, where a parser will
want the array.

**Keyword table.** A module-level `let _KW = kw_table()` holding a `HashTable`
of all 48 spellings, with `func kw_table()` declared **above** it for the
reason in section 7 (this is the documented `_private` module-singleton
pattern that `random.mx` already uses, so `AGENTS.md` permits it). Expose it
through `classify(word)`, never as `lexer._KW` (snapshot rule).

## 9. Number scanning, with the two bad-number diagnostics

Entered when the lead character is a digit. The steps are the table order of
4.3, made explicit:

1. Consume the leading digit run.
2. If the next character is `.` **and** the one after it is a digit, consume
   the fraction digits too. A `.` with no digit behind it is remembered, not
   consumed.
3. If the next character is a letter or `_`, emit **BAD_NUM_SUFFIX**: consume
   the numeric run, then the identifier run, and raise. This is tried first
   because it is entry 39 - ahead of FLOAT.
4. Otherwise, if a fraction was consumed, emit `FLOAT`.
5. Otherwise, if a trailing `.` was remembered, emit **BAD_FLOAT**.
6. Otherwise emit `INT`.

The message for BAD_NUM_SUFFIX splits the lexeme with `\d+(?:\.\d+)?`, which -
unlike the suffix pattern - demands digits *after* the dot. That asymmetry is
visible in the output and must be ported, not "tidied":

| source | outcome |
|---|---|
| `42` | `INT`, lexeme `42` (value 42) |
| `007` | `INT`, lexeme `007` (value 7 - leading zeros are legal) |
| `1.5` | `FLOAT` |
| `01.5` | `FLOAT`, lexeme `01.5` |
| `1.5.2` | `FLOAT 1.5`, then `DOT`, then `INT 2` |
| `.5` | `DOT` then `INT 5` - there is no leading-dot float |
| `1.` | BAD_FLOAT: `Invalid float '1.' - trailing dot. Use '1' or '1.0'` |
| `1..2` | BAD_FLOAT on `1.` (the next char is `.`, not a digit) |
| `12abc` | BAD_NUM_SUFFIX: `Write '12' and 'abc' separately` |
| `1.5y` | BAD_NUM_SUFFIX: `Write '1.5' and 'y' separately` |
| `1.y` | BAD_NUM_SUFFIX: `Write '1' and '.y' separately` - the dot goes with the tail |
| `1_.5` | BAD_NUM_SUFFIX: `Write '1' and '_' separately`, lexeme `1_` |
| `1e5` | BAD_NUM_SUFFIX: `Write '1' and 'e5' separately` - exponents do not exist |

## 10. String scanning and the escape set

Exactly eight single-character escapes, plus `\u`:

| escape | value | | escape | value |
|---|---|---|---|---|
| `\\` | backslash | | `\b` | U+0008 |
| `\"` | `"` | | `\f` | U+000C |
| `\/` | `/` | | `\n` | U+000A |
| `\t` | U+0009 | | `\r` | U+000D |
| `\uXXXX` | code point, 4 hex digits, case-insensitive | | | |

Everything else is invalid, including the ones people expect: `\0`, `\x41`,
`\#`. Real output from this tree, quoted as evidence:

    [Lexer] Line 2, col 15: Invalid string literal - invalid escape '\#'

Scanning, from the character after the opening quote:

1. ordinary character (not `"`, not `\`, not a newline) - append, continue.
2. `\` - look at the next character: `u` requires four hex digits or the
   reason is `'\u' must be followed by 4 hex digits`; a listed escape appends
   its value; anything else gives `invalid escape '\<c>'`. Step 6 characters
   for `\u`, 2 otherwise - the stepper must match, because the reason search
   below steps the same way.
3. `"` - close; the token is `STRING`, value = decoded text.
4. newline or end of source - the literal is **unterminated**.

A literal can never contain a newline, so it can never span lines. When the
fragment is unterminated, `lexer.py` first runs `_bad_escape_reason` over the
fragment (skipping the opening quote) and reports the **first** bad escape it
finds; only when the fragment has no escape problem does it report
`Unterminated string literal (missing closing " before end of line)`. Bad
escape outranks unterminated - e.g. `"a\q` reports the escape, not the missing
quote.

## 11. Lines and columns

`lexer.py` recomputes per token: `line` = newlines before the match start + 1;
`column` = offset from the end of the previous newline, 1-based.

- Both are **1-based**, and a **tab is one column** - a column is a character
  offset, not a visual width. No tab expansion anywhere.
- `\r` is whitespace for `SKIP` but is *not* a line break: only `\n` advances
  the line counter, so in a CRLF file the `\r` still occupies a column and a
  lone-CR (classic Mac) file is one long line. Reproduce this; do not
  "normalize" line endings.
- Offsets are code-point offsets, and MountX strings index by code point too
  (section 7), so the two agree without conversion.
- The port keeps running `line`/`column` while consuming characters instead of
  rescanning the prefix. That is equivalent precisely because no token spans a
  newline; the invariant is stated in the code so a future multi-line token
  cannot break it silently.
- The `EOF` token is positioned at `newlines + 1` and `length of the last line
  + 1` - i.e. just past the last character. For `a\n` that is line 2, column 1.
- **Quirk that must be copied, not fixed:** `lex` short-circuits before the
  loop when the source is empty or all whitespace, returning a single `EOF`
  at line 1, column 1. So a source of `\n\n` yields `EOF(1,1)` - the general
  formula would have said `(3,1)`. The port needs an explicit early return for
  this, and the corpus needs the case.

## 12. Diagnostic catalog

Six reachable messages - counted by running them, not by counting `raise` sites,
which is how the original draft of this section came out one too high. The text is
copied character for character from `lexer.py`; the samples below were produced by
running this tree, not typed from memory. `{L}`/`{C}` are 1-based.

**12.1 bad numbers**

    [Lexer] Line {L}, col {C}: Invalid float '{V}' - trailing dot. Use '{V-without-dot}' or '{V}0'
    [Lexer] Line {L}, col {C}: Invalid number '{V}' - a number cannot be followed by letters. Write '{num}' and '{rest}' separately

Observed: `Invalid float '1.' - trailing dot. Use '1' or '1.0'` and
`Invalid number '1_' - a number cannot be followed by letters. Write '1' and
'_' separately`. Note `'1_'`: the quoted lexeme is the consumed run, while
`num`/`rest` come from the `\d+(?:\.\d+)?` split (section 9).

**12.2 unterminated / bad-escape strings**

    [Lexer] Line {L}, col {C}: Invalid string literal - '\u' must be followed by 4 hex digits
    [Lexer] Line {L}, col {C}: Invalid string literal - invalid escape '\{c}'
    [Lexer] Line {L}, col {C}: Unterminated string literal (missing closing " before end of line)

Observed: `Invalid string literal - invalid escape '\q'` and
`Invalid string literal - '\u' must be followed by 4 hex digits`.

**12.3 unexpected character**

    [Lexer] Line {L}, col {C}: Unexpected character {py_repr(c)}

`py_repr` is Python's `repr` of a one-character string, and it is not as simple
as it looks. Observed, all from one run:

| char | rendered | rule |
|---|---|---|
| `@` | `'@'` | single quotes around a printable ASCII char |
| `'` | `"'"` | the char is itself a single quote -> double quotes take over |
| `\` | `'\\'` | backslash doubled |
| `\x07` | `'\x07'` | control char -> `\xHH`, lowercase hex. Python has short forms only for `\t \n \r`; `\b` and `\f` print as `\x08` / `\x0c` |
| `\u20ac` (euro) | `'\u20ac'` | **printable non-ASCII is printed literally**, not escaped |

So the port needs a `py_repr_char(c)` helper: a decision table over code point
0-255 (the Latin-1 block, where printability is a short range list) plus the
quoting rule above. Section 15 covers how to get code points at all.

**12.4 dead on purpose.** `decode_string` also raises four messages that `lex`
never shows: `Invalid string literal - ends with a lone '\'`, `Invalid string
escape: lone '\' at end of ...`, the `\u` variant, and `Invalid string escape
'\{c}' in ...`. They are unreachable because entry 43 only matches literals whose
escapes are already valid - a literal ending in an unescaped backslash has no
closing quote, so the regex declines it first and the scan path is never entered.
Measured: `"abc\` and `"a\"` both report *Unterminated string literal*, not the
lone-backslash message. The port implements them anyway for `decode_string`'s own
contract, and the harness must **not** expect them from corpus cases - do not chase
them. What the harness must do is report which ones it reached, which is how this
count got fixed: a corpus that never emits a template is either covering dead code
or missing a bug.

## 13. Canonical token line

One line per token, `TYPE|LINE|COL|V`, `EOF` last, first error wins and prints
one `ERR|<exact message>` line:

| token kind | V |
|---|---|
| keyword, `IDENT`, `NULL`, punctuation | the lexeme, verbatim (`nil`, `&&`, `MountX`) |
| `INT`, `FLOAT`, `BAD_*` | the lexeme verbatim (`007`, `01.5`) - per E3, never the parsed value |
| `BOOL` | `true` / `false` |
| `STRING` | `enc(decoded value)` |
| `COMMENT`, `SKIP` | never emitted (discarded, like the oracle) |
| `EOF` | empty |

`enc(s)` exists so that `|` and control characters cannot corrupt the format,
and it must be implemented identically on both sides: `\` -> `\\`, `|` ->
`\p`, newline -> `\n`, tab -> `\t`, CR -> `\r`, any other code point below
0x20 -> `\xHH` lowercase, everything else literal (including non-ASCII, which
matches the `py_repr` finding in 12.3). The Python-side `enc` lives in the
harness, never in `lexer.py`.

## 14. The parity harness

    selfhost/gen_cases.py    writes the corpus into cases/ - one dict, not 22 files
    selfhost/cases/NN-*.txt  one corpus case per file, compared byte-exactly
    selfhost/lexer.mx        the port
    selfhost/parity.mx       MountX driver: the curated corpus and the inline cases
    selfhost/parity_repo.mx  MountX driver: every .mx and .py in the repository
    selfhost/parity.py       oracle driver: same corpus through lexer.lex, same encoder

Gate (run from the repo root; `parity.py` supplies that cwd when it launches the
driver, and `script_dir` is what makes `importx lexer` resolve, per section 2):

    python3 selfhost/parity.py --check          # curated corpus + inline cases
    python3 selfhost/parity.py --repo --check    # every .mx and .py in the repo

`--check` runs the port under `main.py`, captures its stdout as bytes, and diffs
against the oracle stream, printing a unified diff on drift. Without `--check` the
script only prints the oracle stream, which is what you redirect when you want to
look at it by hand. `--repo` walks the tree in Python and hands the port
`repo_files.txt`, because `os_listdir` reads one directory and cannot recurse;
that manifest is shared, the lexing is not.

The V field is the raw lexeme, which `Token` does not keep - `007` converts to the
int 7. `parity.py` recovers raw text by re-scanning with `lexer.TOKEN_REGEX`, the
same object `lex()` uses rather than a second copy of the pattern, and refuses to
print anything if that scan and `lex()` disagree. A harness that quietly
substituted a different string would hide the very bugs it exists to find.

Corpus, seeded from the checks listed in sections 6, 9, 10, 11 and 12 - every
row here is an observed expectation:

| case | expected first lines / error |
|---|---|
| `let\tx` | `LET\|1\|1\|let`, `IDENT\|1\|5\|x` (tab = one column) |
| `true nil && \|\| != ! >= <= ==` | `BOOL\|1\|1\|true`, `NULL\|1\|6\|nil`, `AND\|1\|10\|&&` ... `EQEQ\|1\|27\|==` |
| `letter nilx truex and_or notx MountX` | six `IDENT` lines - the `\b` emulation |
| `1.y` | `ERR\|[Lexer] Line 1, col 1: Invalid number '1.y' - ... Write '1' and '.y' separately` |
| `1_.5` | `ERR` quoting lexeme `1_` |
| `1..2` | `ERR` with `Invalid float '1.'` |
| `"a\q` | `ERR` with `invalid escape '\q'` |
| `"ab` | `ERR` with `Unterminated string literal ...` |
| two blank lines | `EOF\|1\|1\|` - the short-circuit quirk |
| `a` + newline | `IDENT\|1\|1\|a`, `EOF\|2\|1\|` |
| `#c` + newline + `let x = 1` | `LET\|2\|1\|let`, `ASSIGN\|2\|7\|=` |
| one `@` | `ERR` with `Unexpected character '@'` |

Then two escalating stages: **lex the port with the port**, and **lex the whole
repo** against the oracle. The second stage is the one that finds bugs, and it
earned its keep immediately. Measured at M1: the curated corpus is 22 generated
cases plus 30 inline ones, 255 lines, green; the repo stage is 42 files, 34,685
lines, green, in about four and a half minutes. Two defects reached the repo stage
and not the curated one - a set of keyword token names transcribed from this
document instead of read off `match.lastgroup` (`lenx` is `LEN`, `selfx` is
`THISX`, `nil` is `NULL`, `throw` is `THROWX`), and the `char_of` limit firing on a
`\u20ac` in a test file's own source. The names now come out of `TOKEN_SPEC`
programmatically rather than from anyone's memory.

Two divergence classes are named rather than skipped, so a failure explains
itself: **D5** lists the words Python's `\w` accepts and the port does not, **D3**
lists the string literals carrying `\uXXXX` escapes above U+00FF. Both print on the
failure path only. A corpus that trips one either changes, or the decision gets
reopened - what it must not do is pass quietly.

One trap in the harness itself: reading case files through text-mode I/O
normalizes CRLF to LF on **both** sides (`readx` does it, `open()` does it), so
file-based cases cannot express CRLF sources. The CRLF and lone-CR cases from
section 11 live in `INLINE` in `parity.py` and in `inline_cases()` in `parity.mx`,
spelled the same way in both. That duplication is deliberate: only the oracle side
is generated, and the port has to reach the same characters by its own means.

## 15. The one missing primitive, and how to live without it

`\uXXXX` needs characters from code points and vice versa. **MountX has no
`chr`/`ord`, and `interpreter.py` has no `char_code` / `char_from` native**
(grepped, not assumed), and phase 1 promises no new natives. The workaround is
pure MountX:

- A module-level constant holding code points 0x00-0xFF as one escaped string
  literal (about 1.5 KB of `\uXXXX` escapes). Ironically useful: writing it is
  only possible because entry 43 supports `\u`.
- **code -> char**: index that table. `s[c]`, code-point indexing, exact.
- **char -> code**: `string.index_of(TABLE, c)` - the same trick
  `string.tonum` uses against `"0123456789"` (verified: returns `7` for `"7"`,
  `-1` when absent). Linear in 256; called only on escape decoding and
  `py_repr`, never per character.
- `py_repr_char(c)` then rides on the same table plus the quoting rules from
  12.3.

Coverage stops at U+00FF. Above it, `index_of` returns -1 and the port must
fail loudly rather than invent a character (decision D3). This is not
hypothetical: the first repo-wide run died on a `\u20ac` written into `parity.mx`
to spell the euro case, which is why the corpus spells non-ASCII characters
literally instead. A literal character costs nothing - it is a slice of the source,
never a synthesis - so the gap is `\uXXXX` above U+00FF and nothing wider.
`char_from` as a native would close it, and that is the alternative D3 records.

## 16. Cost model, stated before the fact

| activity | cost | note |
|---|---|---|
| dispatch per character | O(1) | one lead-character test, then a short chain |
| lexeme accumulation | O(k^2) in lexeme length | lexemes are short; a 200-char string literal is 200 appends |
| token array | O(n^2) in token count | `xs = xs + [v]` copies (section 7) |
| reference measurement | ~800 appends in about 1 ms | measured in `examples/tour.mx` |

`fbtest.mx` is ~20 KB and roughly 6000 tokens, so the naive `lex()` array path
is on the order of tens of millions of element copies - seconds, not
milliseconds. Measured, though, `lex_print` and `render(lex(src))` take the same
38 s on that file and print the same 6078 lines: the array rebuild is not where
the time goes. It goes into the interpreter stepping through the dispatch chain
once per character, which is what section 6's lead-character design was for. So
chunked accumulation is not worth writing until something slower than this is
asked of it; if it ever is, that is the fix (fill a block of 64, append the block
once), and no interpreter change is needed for it.

## 17. Decisions register - your call, each one changes the code

| # | issue | recommendation | alternative |
|---|---|---|---|
| D1 | keyword lookup structure | module `HashTable` singleton, builder declared above the `let` (section 8) | flat `if` chain; measure both at M1 |
| D2 | number equality | lexeme equality (E3) | value equality, which needs a correctly-rounding parse native |
| D3 | `\uXXXX` above U+00FF | out of scope, loud failure, recorded divergence | add a `char_from` native (breaks the no-new-natives rule) |
| D4 | `py_repr_char` coverage | table over 0x00-0xFF, corpus restricted to it | full Unicode printability tables - disproportionate |
| D5 | Unicode word characters | ASCII-only in the port; divergence is loud, not silent | narrow `lexer.py`'s `\w` to ASCII so both agree - a real behaviour change, needs its own approval |
| D6 | whitespace-only short-circuit | copy it exactly (section 11) | "fix" it in `lexer.py` too, which changes current behaviour |
| D7 | how errors are compared | message strings (E2) | match process stderr - impossible, and pointless |
| D8 | corpus output shape | `lex_print` streaming, because MountX has no callbacks | array only, and pay section 16 |
| D9 | where the code lives | `selfhost/`, driver inside it, parity run as its own command | `libs/lexer.mx`, which **is** in the import search path - so `fbtest.mx` could assert against it and parity would join the existing gate, at the cost of putting a demo in a production search path |

D9 is the one with a long-term consequence: today `fbtest.mx` is the gate, and
D9 decides whether self-hosting is inside that gate or beside it.

## 18. Milestones

| stage | deliverable | exit criterion | status |
|---|---|---|---|
| M0 | this document | you approve it, or hand back edits to D1-D9 | delivered |
| M1 | `lexer.mx` + `cases/` + both drivers | `diff` empty over the seeded corpus, all six reachable diagnostics reached | green - 52 cases, 255 lines, six of six |
| M2 | self-lexing | the port lexes `selfhost/lexer.mx` and matches the oracle token for token - including its own `\uXXXX` table literal | green - inside the repo run |
| M3 | whole repo | 27 stdlib modules, `fbtest.mx`, `examples/tour.mx`, `lexer.py`-adjacent `.mx` files: empty diff, wall time recorded | green - 42 files, 34,685 lines, ~4.5 min |
| M4 | diagnostics hardening | every reachable template in section 12 reached; every `py_repr` row in 12.3 exercised, not just the five sampled | open - 70 of 256 code points exercised |
| M5 | *(optional, needs separate approval)* | MountX lexer behind a flag in `main.py`; explicitly **not** phase 1 | not started |

## 19. If this ships, the docs follow

- `README.md`: one paragraph under Examples plus a `selfhost/` line in Project
  layout. Not in the 27-module table - it is not stdlib.
- `AGENTS.md`: the parity command joins the test protocol, with the rule "any
  change to `lexer.py` must re-run the parity gate". That is the real payoff
  for the project: `lexer.py` currently has no differential test at all.
- `fbtest.mx`: untouched unless D9 goes to `libs/`, in which case the header
  count moves per the existing rule.
- **No VERSION bump**: phase 1 changes nothing a user can observe.

## 20. Open questions

1. D9 - should parity join the `fbtest.mx` gate, and is `libs/` an acceptable
   home for a demo to earn that?
2. D5 - loud divergence on Unicode identifiers for now, or do you want
   `lexer.py` narrowed to ASCII word characters as a deliberate behaviour
   change?
3. D3 - accept "`\uXXXX` up to U+00FF only", or approve one native
   (`char_from`) and let the no-new-natives rule go?
4. M3 - is lexing the entire repo as the decisive gate acceptable at seconds
   per run, or should it be a nightly-style check kept out of the fast loop?
5. After M4 - do you want the MountX lexer actually wired in behind a flag
   (that is an interpreter change, phase 2), or does it stay an oracle
   fixture and a demonstration?

## 21. What this phase will not touch

`lexer.py`, `parser.py`, `main.py`, `interpreter.py`, `fbtest.mx`, `stdlib/`.
New files only: `selfhost/lexer.mx`, `selfhost/parity.mx`,
`selfhost/parity.py`, `selfhost/cases/*`. Two doc files gain a paragraph.
