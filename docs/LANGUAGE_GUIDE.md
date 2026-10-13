<!-- Copyright (C) 2026-2027 Ziad Mohsin. All Rights Reserved. -->
# The MountX Language Guide

The complete reference. Every example here is executed by `docs_verify.py`, so
the output shown is the output the language produces.

New to MountX? Read [TUTORIAL.md](TUTORIAL.md) first. This document assumes you
can write a function and a class.

---

## Contents

1. [Running programs](#running-programs)
2. [Syntax cheatsheet](#syntax-cheatsheet)
3. [Values and equality](#values-and-equality)
4. [Control flow](#control-flow)
5. [Functions](#functions)
6. [Arrays](#arrays)
7. [Classes](#classes)
8. [Exceptions](#exceptions)
9. [Modules](#modules)
10. [Native builtins](#native-builtins)
11. [The standard library — all 27 modules](#the-standard-library--all-27-modules)
12. [Errors](#errors)
13. [Common mistakes](#common-mistakes)
14. [Known limitations](#known-limitations)

---

## Running programs

| Command | What it does |
|---|---|
| `mountx prog.mx` | Run a file |
| `mountx` | REPL — state persists across lines; `exit`, `quit` or Ctrl-D leaves |
| `mountx -e "printx(1+1)"` | Run a snippet; the `mountx` wrapper is added for you |
| `mountx --version` / `-v` | Print the version, exit 0 |
| `mountx --help` / `-h` | Print usage, exit 0 |
| `mountx --selfhost prog.mx` | Run the program through the self-hosted `interp.mx` |

Exit code is `0` on success and `1` for any error — syntax, runtime, uncaught
`throwx`, unreadable file, unknown option.

`mountx` is a wrapper that finds the repository and hands off to `main.py`,
forwarding every argument untouched. To install it:

```bash
ln -s "$PWD/mountx" ~/.local/bin/mountx
```

It resolves through symlinks, so the link works from any directory.
`python3 main.py prog.mx` remains supported and does the same work.

### The self-hosted front end

MountX is implemented in MountX. `selfhost/interp.mx` is a complete
interpreter written in the language it interprets, and running a program
through it produces byte-identical output:

```bash
mountx --selfhost fbtest.mx
```

The wrapper sets `$MOUNTX_STD` and points `main.py` at `interp.mx`; `main.py`
itself is unchanged. On `fbtest.mx` that means the same 365 bytes and exit 0
in **0.47 s** on the fast path versus **183 s** on the self-hosted one —
about 400x, the cost of interpreting an interpreter. `--selfhost` is a
verification switch, not the default.

The REPL keeps one interpreter alive, so this works:

```
mountx
let xs = [1, 2, 3]
printx(len(xs))
unmountx
```
```out
3
```

---

## Syntax cheatsheet

Everything on one page. Each construct is shown in context later in this guide.

```
mountx                                  # file wrapper (auto-added if absent)
unmountx

# comments
# this is a comment

# variables and assignment
let x = 10
x = 20                                  # no second keyword for assignment

# output and input
printx("x = " + x)
let name = inputx("name? ")

# control flow
if x > 5 { } else if x > 0 { } else { }
while cond { }
loop 10 { }                             # fixed count
for v in arr { }                        # arrays and strings
for i, v in arr { }                     # with index
break
continue

# functions
func add(a, b) { return a + b }
func nothing() { return }               # returns null

# classes
classx Animal {
    let legs = 4                        # field default
    func init(name) { this.name = name }
    func speak() { return "..." }
}
classx Dog extends Animal {
    func init(name) { super.init(name) }        # or bare: super(name)
    func speak() { return super.speak() + " woof" }
}
let d = newx Dog("Rex")                 # newx module.Class(...) also works
d.legs = 3                              # field assignment
this                                    # current instance, inside a method

# exceptions
tryx { throwx "bad" } catchx(e) { printx(e) }

# modules
importx string
printx(string.upper("hi"))
importx stdlib                          # everything, callable bare

# files
let txt = readx("a.txt")
writex("a.txt", "hi")
appendx("a.txt", "!")
exportx("a.json", [1, 2])               # JSON dump

# builtins
len([1, 2, 3])
typeofx(1.5)
idiv(7, 2)
```

**Keyword aliases.** Every keyword has an `x` form, and they are interchangeable:
`class`/`classx`, `new`/`newx`, `null`/`nullx`/`nil`, `len`/`lenx`/`length`,
`throw`/`throwx`, `try`/`tryx`, `catch`/`catchx`, `import`/`importx`,
`while`/`whilex`, `loop`/`loopx`, `for`/`forx`, `func`/`funcx`, `return`/`returnx`,
`if`/`ifx`, `else`/`elsex`, `break`/`breakx`, `continue`/`continuex`, `extends`/`extendsx`.

The standard library uses the `x` forms consistently.

---

## Values and equality

Eight kinds: `int`, `float`, `string`, `bool`, `null`, `array`, instances,
`module`, `function`.

Precedence, loosest first:
`or < and < comparison < additive < multiplicative < unary < postfix`

```mountx
mountx
printx(1 + 2 * 3)
printx((1 + 2) * 3)
printx(2 + 3 == 5)
printx(!true && false || true)
printx(-5 + 3)
unmountx
```
```out
7
9
true
true
-2
```

### The rules that surprise people

**`/` always returns a float.** Always. Even `6 / 2` is `3.0`.

```mountx
mountx
printx(6 / 2)
printx(idiv(6, 2))
printx(idiv(-7, 2))
unmountx
```
```out
3.0
3
-3
```

`idiv` truncates toward zero. This is not incidental — `mathfull`, `bst`,
`hashtable` and `time` all depend on it.

**A boolean is not a number.** In Python `bool` subclasses `int`; in MountX the
two are separate types and `is_num` guards against Python's hierarchy.

```mountx
mountx
printx(1 == 1.0)
printx(true == 1)
printx(false == 0)
unmountx
```
```out
true
false
false
```

**Cross-type equality is always false.** `"1" == 1` is `false`, not an error and
not `true`.

**Arrays compare deeply; instances compare by identity.**

```mountx
mountx
classx P { func init() {} }
let a = newx P()
let b = newx P()
let c = a
printx([1, [2]] == [1, [2]])
printx(a == b)
printx(a == c)
unmountx
```
```out
true
false
true
```

**Truthiness.** Falsy: `null`, `false`, `0`, `""`, `[]`. Truthy: everything else,
including `1`, `"x"` and non-empty arrays. Note `[]` is falsy — unlike Python.

**String concatenation uses display form.** `+` with a string on the left
converts the right side via `display()`, which renders `true`, `null`, `[1, 2]`
and cycles.

```mountx
mountx
printx("n=" + nullx)
printx("f=" + 1.5)
printx("b=" + true)
unmountx
```
```out
n=null
f=1.5
b=true
```

---

## Control flow

```mountx
mountx
let n = 7
if n % 2 == 0 {
    printx("even")
} else {
    printx("odd")
}

let i = 0
while i < 3 {
    printx("i=" + i)
    i = i + 1
}

loop 3 { printx("once") }

for v in [1, 2] { printx("v=" + v) }
for idx, v in [1, 2] { printx(idx + "/" + v) }

for ch in "ab" { printx(ch) }
unmountx
```
```out
odd
i=0
i=1
i=2
once
once
once
v=1
v=2
0/1
1/2
a
b
```

`%` is modulo. Note `for ... in` iterates **arrays and strings only** — not
dicts, not ranges. Use `iter.range` for that.

`break` exits the innermost loop, `continue` skips an iteration:

```mountx
mountx
for n in [1, 2, 3, 4, 5] {
    if n == 3 { continue }
    if n == 5 { break }
    printx(n)
}
unmountx
```
```out
1
2
4
```

There is no `switch` and no `do`/`while`. Use `if`/`else if` chains.

---

## Functions

```mountx
mountx
func add(a, b) { return a + b }
func noret() { return }
func greet(name, greeting) {
    printx(greeting + ", " + name)
}

printx(add(2, 3))
printx(typeofx(noret()))
greet("you", "Hello")
unmountx
```
```out
5
null
Hello, you
```

A function returning nothing yields `null`. Arguments are positional; there are no
keyword arguments, defaults, or varargs.

**Recursion works**, and depth is counted — a program that recurses past the
interpreter's ceiling is refused with a catchable error rather than crashing.

**No closures.** A function sees its parameters, its module scope, and globals —
not the scope where it was defined. To capture a value, pass it as an argument.

Functions are values, so you can store and pass them:

```mountx
mountx
importx array
func dbl(x) { return x * 2 }
printx(array.map([1, 2, 3], dbl))
printx(typeofx(dbl))
unmountx
```
```out
[2, 4, 6]
function
```

There is **no lambda literal**. A function must be declared with `func` and then
referred to by name.

---

## Arrays

```mountx
mountx
let xs = [1, 2, 3]
printx(len(xs))
printx(xs[0])
printx(xs[1] + xs[2])
xs[1] = 20
printx(xs)
printx(xs + [4])
printx("nested: " + [[1, 2], [3]])
unmountx
```
```out
3
1
5
[1, 20, 3]
[1, 20, 3, 4]
nested: [[1, 2], [3]]
```

`+` on arrays returns a **new** array; it does not mutate. To append, assign the
result back. Indices are non-negative — `xs[-1]` is an error, not "the last
element".

Slicing is a function call, not syntax:

```mountx
mountx
importx array
let xs = [0, 1, 2, 3, 4]
printx(array.slice(xs, 1, 3))
printx(array.slice(xs, 0, 2))
unmountx
```
```out
[1, 2]
[0, 1]
```

The `array` module has `map`, `filter`, `reduce`, `first`, `find`, `slice`,
`contains`, `index_of`, `reverse`, `sum`, `min`, `max`. The `arrayfull` module
adds `sort_by`, `unique`, `flatten`, `zip`, `chunk`, `take`, `drop`.
---

## Classes

```mountx
mountx
classx Animal {
    let legs = 4
    func init(name) { this.name = name }
    func speak() { return "..." }
    func describe() { return this.name + " has " + this.legs + " legs" }
}

let a = newx Animal("generic")
printx(a.describe())
printx(a.speak())
printx(typeofx(a))
a.legs = 3
printx(a.describe())
unmountx
```
```out
generic has 4 legs
...
Animal
generic has 3 legs
```

`typeofx` on an instance gives the **class name**. There is no `classof` builtin —
for a real type test use the `type` module's `is_instance`.

Inheritance, with two forms of `super`:

```mountx
mountx
classx Animal {
    let legs = 4
    func init(name) { this.name = name }
    func speak() { return "..." }
}

classx Dog extends Animal {
    func init(name) { super.init(name) }
    func speak() { return super.speak() + " woof" }
}

classx Puppy extends Dog {
    func speak() { return super.speak() + "!" }
}

printx(newx Dog("Rex").speak())
printx(newx Puppy("Bit").speak())
unmountx
```
```out
... woof
... woof!
```

Three rules:

- **Fields merge parent-first**, child overrides. The nearest `init` up the chain
  runs.
- **A subclass with no `init` inherits its parent's**, so `classx Dog extends
  Animal {}` constructs fine.
- **Bare `super(...)` calls the nearest `init` up the chain**, which is what you
  want in a three-level hierarchy.

`super` resolves through the *defining* class, not the receiver's — that is why
`Puppy` above gets `Dog`'s `speak`, not `Animal`'s.

A parent may be module-qualified (`classx MyList extends dict.Dict`), which is how
you subclass something out of the standard library.

---

## Exceptions

```mountx
mountx
tryx {
    throwx "bad thing"
} catchx(e) {
    printx("caught: " + e)
}
printx("after")
unmountx
```
```out
caught: bad thing
after
```

`throwx` passes its value through **unchanged** — a string, a number, an array, an
instance. Runtime errors are the ones that arrive as strings:

```mountx
mountx
tryx {
    let z = 1 / 0
    printx(z)
} catchx(e) {
    printx(typeofx(e) + ": " + e)
}
unmountx
```
```out
string: Division by zero
```

**`return`, `break` and `continue` are never caught.** They use a separate
mechanism and `tryx` re-raises them, so control flow inside a `tryx` behaves as
you would expect. There is no `finally`.

For value-style error handling use the `error` module:

```mountx
mountx
importx error
func risky() { throwx "nope" }
let r = error.attempt(risky)
printx(typeofx(r[0]))
printx(r[0] == "err")
printx(r[1])
unmountx
```
```out
string
true
nope
```

---

## Modules

```mountx
mountx
importx string
importx math
printx(string.upper("hi"))
printx(string.trim("  pad  "))
printx(math.max(3, 9))
printx(math.abs(-4))
unmountx
```
```out
HI
pad
9
4
```

`importx stdlib` imports everything, callable bare:

```mountx
mountx
importx stdlib
printx(factorial(5))
printx(string.upper("via module"))
unmountx
```
```out
120
VIA MODULE
```

**Where `importx` looks.** For each base directory in order, it tries
---

## Native builtins

Seventeen functions implemented directly in `interpreter.py`. Their names use
underscores so they cannot collide with a user function or a stdlib name.

| Native | Does |
|---|---|
| `len(x)` | length of an array or string |
| `printx(v)` | print one value |
| `inputx(prompt?)` | read one line from stdin |
| `readx(path)` | whole file as a string |
| `writex(path, text)` | write a file (creates or overwrites) |
| `appendx(path, text)` | append to a file |
| `exportx(path, value)` | write `value` as JSON |
| `typeofx(v)` | type name; for an instance, the class name |
| `idiv(a, b)` | integer division, truncating toward zero |
| `math_floor(x)` | floor of a number, as an int |
| `mx_version()` | the language version string |
| `time_now_ms()` | milliseconds since the epoch |
| `sleep_ms(ms)` | sleep |
| `sys_args()` | command-line arguments as an array |
| `sys_exit(code)` | exit immediately, not catchable |
| `os_env(name)` | environment variable, or null |
| `os_cwd()` | current working directory |
| `os_listdir(path)` | directory entries |
| `os_sep()` | the OS path separator |
| `http_get(url)` | fetch a URL as text |

The four SQLite natives — `db_open`, `db_close`, `db_exec`, `db_query` — exist for
the `db` module. Use the module, not the natives.

```mountx
mountx
printx(len("abc"))
printx(mx_version())
printx(math_floor(2.7))
printx(typeofx([]))
printx(typeofx(nullx))
unmountx
```
```out
3
1.1.0
2
array
null
```

`mx_version()` returns the bare version string; `--version` is what prefixes
`MountX ` onto it.

---

## The standard library — all 27 modules

27 importable modules, organised into category folders. `importx <name>` finds
them all.

### core

**`type`** — type predicates and checks.

*Useful: `is_num(v)`, `is_instance(v, "ClassName")`, `to_bool(v)`, `deep_copy(v)`.*

**`error`** — errors as values.

*Useful: `MxError`, `attempt(fn) -> ["ok",v] / ["err",e]`, `retry(fn, n)`.*

**`iter`** — ranges and sequence helpers.

*Useful: `range(n)`, `range_step(a, b, n)`, `zip(a, b)`, `chunks(arr, n)`,
`scan(arr, fn)`.*

**`assert`** — test helpers, used by `fbtest.mx` itself.

*Useful: `assert_eq(a, b)`, `assert_throws(msg, fn)`, `Tracker` with
`check`/`check_eq`/`summary`.*

**`random`** — seeded pseudo-randomness, so runs are reproducible.

*Useful: `Rand` (an LCG), `seed_rng(n)`, `rand_below(n)`, `shuffle(arr)`,
`choice(arr)`.*

```mountx
mountx
importx random
random.seed_rng(42)
let first = random.rand_below(1000)
random.seed_rng(42)
let second = random.rand_below(1000)
printx(first == second)
unmountx
```
```out
true
```

### data

**`dict`** — an ordered dictionary built on parallel arrays. `keys` and `values`
are **fields**, not methods.

*Useful: `Dict` with `set(k,v)`, `get(k)`, `has(k)`, `remove(k)`, `size()`,
`clear()`.*

```mountx
mountx
importx dict
let d = newx dict.Dict()
d.set("b", 1)
d.set("a", 2)
printx(d.has("a"))
printx(d.get("a"))
printx(d.keys)
printx(d.size())
unmountx
```
```out
true
2
["b", "a"]
2
```

**`map`** — `Map` plus `pairs_*` helpers, for the JSON object shape
`[[k,v],...]`.

*Useful: `Map`, `pairs_get(pairs, k)`, `pairs_set`, `pairs_keys`.*

**`set`** — mathematical sets.

*Useful: `Set`, `union(s, t)`, `intersection(s, t)`, `difference(s, t)`,
`is_subset(a, b)`.*

**`hashtable`** — chained, auto-rehashing, any key.

*Useful: `HashTable`, `set(k, v)`, `get(k)`, `has(k)`, `remove(k)`, `size()`.*

### structs

**`stack`**, **`queue`**, **`linkedlist`**, **`bst`** — the classic structures.
`bst` is an ordered map, so in-order traversal gives sorted keys.

*Useful: `push`/`pop` (stack), `enqueue`/`dequeue` (queue), `add_first`/`add_last`
(list), `insert`/`find`/`inorder` (bst).*

**`collections`** — one import that re-exports all five.

### text

**`string`** — the workhorse.

*Useful: `split(s, sep)`, `join(arr, sep)`, `index_of(s, sub)`,
`replace_all(s, a, b)`, `trim(s)`, `tonum(s)`, `pad_left(s, n)`.*

```mountx
mountx
importx string
printx(string.split("a,b,c", ","))
printx(string.join(["x", "y"], "-"))
printx(string.index_of("hello", "ll"))
printx(string.replace_all("a-b", "-", "+"))
printx(string.trim("  pad  "))
printx(string.tonum("42") + 1)
unmountx
```
```out
["a", "b", "c"]
x-y
2
a+b
pad
43
```

**`json`** — parse and serialise. Objects are `[[key, value], ...]` pairs, which
is why `json` and the `map` module agree on shape.

*Useful: `stringify(v)`, `parse(text)`, `try_parse(text)`.*

**`array`** — functional basics.

*Useful: `map(arr, fn)`, `filter(arr, fn)`, `reduce(arr, fn, init)`,
`first(arr)`, `find(arr, fn)`, `slice(arr, s, e)`.*

**`arrayfull`** — the extended set.

*Useful: `sort_by(arr, cmp)`, `unique(arr)`, `flatten(arr)`, `chunk(arr, n)`,
`take(arr, n)`, `drop(arr, n)`.*
`<base>/stdlib/<category>/<name>.mx`, `<base>/stdlib/<name>.mx`,
`<base>/libs/<name>.mx` and `<base>/<name>.mx`. First hit wins. The bases are:

1. `$MOUNTX_STD`, if set
2. the directory of the script being run (`-e` has none)
3. the current working directory
4. the directory holding `interpreter.py`

Base 4 is why a program anywhere on your filesystem can `importx string`. Base 2
is why a module sitting next to your script is importable regardless of where you
run from.

**Two things to know about module semantics:**

- Function names land in the **global namespace**, so two modules defining the same
  function name means last import wins. Prefer `module.function`.
- Module *objects* are protected from being shadowed — but functions are not.
  Never name a stdlib function after a module it lives in.

---

## Errors

The CLI formats every error uniformly.

| Prefix | Meaning |
|---|---|
| `[Lexer] ...` | lexing problem, with line and column |
| `[Parser] ...` | parsing problem, with line and column |
| `[Syntax] ...` | the few lexer checks that carry no position |
| `[Runtime] ...` | a runtime error, or an uncaught `throwx` |

An uncaught `throwx` prints as `[Runtime] Uncaught throwx: <value>` — there is no
separate `throwx` prefix, and the value is rendered with `display()`. It goes to
**stdout**, not stderr, and the process exits `1`:

```mountx exit=1
mountx
printx("before")
throwx [1, "two", true]
unmountx
```
```out
before
[Runtime] Line 3: Uncaught throwx: [1, "two", true]
```

Inside `catchx`, the thrown value is whatever you threw:

```mountx
mountx
tryx {
    throwx [1, 2]
} catchx(e) {
    printx(typeofx(e) + " -> " + e)
}
unmountx
```
```out
array -> [1, 2]
```

---

## Common mistakes

Every one of these is something that was actually got wrong while building the
language or its self-hosting interpreter, and several are recorded in
`AGENTS.md` under "Historical bug log".

### 1. Expecting `/` to give an integer

```mountx
mountx
printx(7 / 2)
printx(7 / 2 == 3)
unmountx
```
```out
3.5
false
```

`/` always returns a float. Use `idiv(7, 2)`. Half the standard library depends
on this.

### 2. Treating a boolean as a number

```mountx
mountx
printx(true == 1)
unmountx
```
```out
false
```

In Python `bool` subclasses `int`, so this mistake is easy to make while reading
the interpreter source. MountX separates them, and `is_num` guards against
Python's hierarchy explicitly (`interpreter.py:51-52`). Adding a bool to a
number is a runtime error.

### 3. Using `xs[-1]` for the last element

```mountx exit=1
mountx
printx([1, 2, 3][-1])
unmountx
```
```out
[Runtime] Line 2: Index -1 out of range (length 3)
```

Indices are non-negative. Use `xs[len(xs) - 1]`.

### 4. Writing a lambda literal

```mountx exit=1
mountx
importx array
printx(array.map([1], func x { return x + 1 }))
unmountx
```
```out
[Parser] Line 3, col 23: Unexpected FUNC ('func') inside expression
```

MountX has no lambda syntax. Declare a named function and pass it by name.

### 5. Forgetting that `keys` is a field

```mountx exit=1
mountx
importx dict
let d = newx dict.Dict()
d.set("a", 1)
printx(d.keys())
unmountx
```
```out
[Runtime] Line 5: 'keys' is not callable
```

`d.keys` is the array. `Dict` keeps `keys` and `values` as parallel arrays and
exposes `set`/`get`/`has`/`remove`/`size` as methods.

### 6. Returning a boolean from a comparator

```mountx exit=1
mountx
importx arrayfull
func cmp(a, b) { return a < b }
printx(arrayfull.sort_by(["b", "a"], cmp))
unmountx
```
```out
[Runtime] Line 302 in stdlib/text/arrayfull.mx: Cannot compare bool and int
```

`sort_by`'s comparator returns a **number** (`-1`, `0`, `1`). MountX's `<` returns
`true`/`false` where Python's returns an int.

### 7. Getting `reduce`'s arguments wrong

```mountx exit=1
mountx
importx array
func addto(acc, x) { return acc + x }
### 8. Forgetting `unmountx`

```mountx exit=1
mountx
printx("oops")
```
```out
[Parser] Line 2, col 15: Program is missing unmountx
```

`main.py` auto-wraps a snippet when `mountx` is absent, but a file that opens with
`mountx` must close it.

### 9. Expecting `tryx` to swallow `return`

```mountx
mountx
func find(xs, t) {
    for i, v in xs {
        tryx { if v == t { return i } } catchx(e) { printx("never") }
    }
    return -1
}
printx(find(["a", "b"], "b"))
unmountx
```
```out
1
```

`return`, `break` and `continue` are control flow, not errors, and `tryx`
re-raises them. The `return` above returns from `find`, not just from the `tryx`.

### 10. Assuming a string concatenates to a number

```mountx exit=1
mountx
printx("a" + 1)
unmountx
```
```out
[Runtime] Unknown operator '+' for 'str' and 'int'
```

`+` concatenates when one side is a string. To convert, use `string.tonum`.

### 11. Trusting a module let as a live handle

```mountx
mountx
importx random
printx(typeofx(random.rand_below))
unmountx
```
```out
function
```

A module's exposed `let` values are a **snapshot** taken at import time. Code
inside the module reads the live module scope; code outside sees the value as it
was when the import happened. So calling `random.seed_rng` does *not* change what
`random._default_rng` points at from your code. Never treat a module-qualified
`let` as a handle.

### 12. Naming a function after the module it lives in

This one cost real debugging time. `arrayfull.mx` once defined a function called
`map`, which silently **destroyed the `map` module object** — the shadowing is
invisible until something tries to use it. The fix was a rule: module objects are
never shadowed, by a function or otherwise. The same hazard bit the self-hosting
interpreter, whose `mk_super` overwrote a `parser.mx` builder of the same name
(renamed to `mk_superbox`).

---

## Known limitations

These are design decisions, not bugs. They are also in `AGENTS.md`.

- **String building is `O(n²)`** — `out = out + s` copies the whole string each
  time. Fine for small strings, bad for large ones.
- **No closures.** Functions see params + module scope + globals.
- **`for ... in` iterates arrays and strings only.** No dict iteration, no ranges;
  use `iter.range`.
- **Module function names are global**, and last import wins among functions.
  Module objects are protected from shadowing; functions are not.
- **Floats print via Python's `str`**, so `0.1 + 0.2` shows float reality.
- **`db.exec` returns `-1` for DDL**, per SQLite's `rowcount` semantics.
- **No `finally`, `switch`, list comprehensions, or lambda literals.**
- **No built-in object/hash literal syntax** — build classes with `classx`.

---

## See also

- **[TUTORIAL.md](TUTORIAL.md)** — the guided introduction.
- **[ARCHITECTURE.md](ARCHITECTURE.md)** — how the interpreter is built.
- **[../README.md](../README.md)** — quickstart and the native list.
- **[../examples/tour.mx](../examples/tour.mx)** — a runnable tour of everything
  on this page.
printx(array.reduce([1, 2, 3], 0, addto))
unmountx
```
```out
[Runtime] 'fn' is not callable
```

The signature is `reduce(arr, fn, init)` — the function comes **before** the
initial value. `map` and `filter` take the function last.
A module-level `let` is visible as `module.name`, but it is a **snapshot** taken at
import time. Code inside the module reads the live scope; code outside sees the
value as it was when the import happened.
