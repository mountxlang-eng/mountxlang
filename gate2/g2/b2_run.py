# Copyright (C) 2026-2027 Ziad Mohsin. All Rights Reserved.
#!/usr/bin/env python3
"""b2_run.py - the B-2 gate: MountX -> compiler.mx -> C -> cc -> run -> diff.

For each case in b1/corpus/: compiler.mx (written in MountX) emits a .c, cc
builds it against mini.c's SUBSTRATE, and the result is compared byte-for-byte
with the SAME goldens B-1 used. One set of goldens, two implementations - that
is what makes the comparison meaningful.

Gates
  G6  the emitted .c compiles with ZERO warnings at -std=c99 -O2 -Wall -Wextra
      -Wclobbered. -Wclobbered on the OUTPUT is the stricter test the brief asks
      for: the generated code is where CB-G's rule actually bites.
  G7  12/12 byte-identical, exit codes equal, same goldens as B-1.
  G8  vacuous-pass refusal: a .c that fails to compile, or a missing compiler,
      is a failure - never a pass.

The stage directory is b2/gen and is gitignored: it is build output.
"""

import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
SELFHOST = os.path.dirname(HERE)
CORPUS = os.path.join(SELFHOST, "b1", "corpus")
B1 = os.path.join(SELFHOST, "b1")
GEN = os.path.join(HERE, "gen")
MAIN = os.path.join(REPO, "main.py")

FLAGS = ["-std=c99", "-O2", "-Wall", "-Wextra", "-Wclobbered"]
# -lm LAST. The linker resolves left to right, so a library listed before the
# object that needs it is discarded - which is how mini.c's fmod (float modulo,
# used by interp.mx:1128) ended up an undefined reference at every link.
LIBS = ["-lm", "-lsqlite3"]
# -lsqlite3 because the db_* natives are REAL as of B-3.5: CB-E ("no
# dependencies beyond libc") was retracted when libsqlite3-dev landed on this
# machine, and db_exec's rowcount -1 for DDL is a shape the corpus asserts. The
# library is a LINK dependency of both gates now, which is a deliberate change
# of CB-E and not an accident - so it is written here rather than discovered by
# a linker error on someone else's box.

# Case 12 must DIVERGE from the host, as in B-1: the host prints a 19-digit
# number, the C port must refuse with CB1's frozen text.
# KNOWN LIMITATIONS - cases the C path does not claim to pass. They are PRINTED,
# never silently dropped, and they are the SAME set as B-1's (b1_run.py): the
# compiled path and the interpreted path share one substrate, so they share one
# list of limitations. A limitation recorded in one gate and not the other would
# mean the two paths disagree about what they can do.
EXPECT_SKIP = {
    "20_darr1_nonascii_slice": {
        "tag": "D-ARR1",
        "why": "string indexing returns one BYTE-sized character; see b1_run.py",
    },
}

EXPECT_DIVERGE = {
    "12_overflow_refusal": {
        "out": "[Runtime] Integer overflow: result does not fit in 64 bits\n",
        "exit": "1",
    },
}


def mx_quote(s):
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


# A module file is NOT a case - the same rule as b1_run.py, for the same reason:
# case 23's helper sits in the corpus directory so the import walk's second base
# is what is under test. "Has a golden, or is a registered skip" is the
# discriminator; a module has neither.
def cases():
    for name in sorted(os.listdir(CORPUS)):
        if not name.endswith(".mx"):
            continue
        stem = name[:-3]
        if not (os.path.exists(os.path.join(CORPUS, stem + ".out")) or
                stem in EXPECT_SKIP):
            continue
        yield stem, os.path.join(CORPUS, name)


def CASE_SRC(stem):
    """The ABSOLUTE path of a case's .mx file.

    Absolute, because the compiler stamps this path into the binary as its
    second import base (mx_set_source) and the binary is run from a different
    directory than the compiler was. A relative path resolves against the
    RUNNER's cwd, so case 23 searched gen/ instead of the corpus directory and
    reported mod_helper.mx missing with a 56-path list naming directories that
    have nothing to do with the program.
    """
    return os.path.join(CORPUS, "%s.mx" % stem)


def emit(stem, src):
    """Run compiler.mx over one case; return the emitted .c text."""
    drv = os.path.join(GEN, "emit_%s.mx" % stem)
    out = os.path.join(GEN, "gen_%s.c" % stem)
    # MountX strings are DOUBLE-quoted; repr() would emit single quotes and the
    # lexer rejects them, so the paths are written with mx_quote() instead.
    open(drv, "w").write(
        'mountx\nimportx compiler\nimportx io\n'
        'let src = io.read_text(%s)\n'
        'io.write_text(%s, compiler.translate(src, %s))\n'
        'unmountx\n'
        % (mx_quote(src), mx_quote(out), mx_quote(CASE_SRC(stem))))
    env = dict(os.environ)
    env["MOUNTX_STD"] = SELFHOST        # so `importx compiler` finds the ports
    p = subprocess.run([sys.executable, MAIN, drv],
                       capture_output=True, env=env, cwd=HERE)
    if p.returncode != 0:
        return None, p.stderr.decode("utf-8", "replace")
    if not os.path.exists(out):
        return None, "compiler.mx produced no file for %s" % stem
    return open(out).read(), None


def substrate():
    """The substrate object, rebuilt when mini.c is NEWER than the object.

    Keyed on mtime rather than existence, because an existence check makes a
    STALE object permanent: edit mini.c, re-run, and every case links against the
    previous substrate. The symptom was a run that failed its FIRST case and then
    passed them all on the second run - which reads like a flaky test and is
    actually a cache with no invalidation. The extra build is a few seconds and
    the gate is worth more than that.
    """
    sub = os.path.join(GEN, "substrate.o")
    src = os.path.join(B1, "mini.c")
    if (not os.path.exists(sub)
            or os.path.getmtime(src) > os.path.getmtime(sub)):
        p = subprocess.run(["cc"] + FLAGS +
                           ["-DMX_SUBSTRATE", "-DMX_BOTH", "-c", "-o", sub,
                            src] + LIBS,
                           capture_output=True, cwd=B1)
        if p.returncode != 0:
            return None, "substrate build failed:\n" + p.stderr.decode()
        w = p.stderr.decode("utf-8", "replace")
        if w.strip():
            return None, "substrate warnings:\n" + w
    return sub, None


def compile_and_link(stem):
    src = os.path.join(GEN, "gen_%s.c" % stem)
    exe = os.path.join(GEN, "prog_%s" % stem)
    if not os.path.exists(src):
        return None, "no generated .c"
    sub, err = substrate()
    if err:
        return None, err
    p = subprocess.run(["cc"] + FLAGS + ["-I", B1, "-o", exe, src, sub] + LIBS,
                       capture_output=True, cwd=HERE)
    if p.returncode != 0:
        return None, p.stderr.decode("utf-8", "replace")
    warn = p.stderr.decode("utf-8", "replace")
    if warn.strip():
        return None, "WARNINGS in the emitted code:\n" + warn
    return exe, None
def main():
    os.makedirs(GEN, exist_ok=True)
    if not os.path.exists(os.path.join(HERE, "compiler.mx")):
        print("B-2 GATE: compiler.mx not found at %s" % SELFHOST)
        print("This is EXPECTED until the emitter exists. See B-2 KICKOFF.md.")
        return 2

    # Build the substrate ONCE, up front, before any case is emitted. It used to
    # be built lazily inside the first case's compile step, and the FIRST case
    # then failed on a cold gen/ while every later case passed - which reads like
    # a flaky gate. It was not flaky: the emitted .c for the first case was being
    # compiled before the object existed. A gate whose first case fails is a gate
    # people stop reading, so the build happens where its failure is unambiguous.
    sub, err = substrate()
    if err:
        print("B-2 GATE FAILED - substrate build:\n%s" % err)
        return 1

    passed = failed = 0
    emitted = 0
    skipped = []
    for stem, src in cases():
        if stem in EXPECT_SKIP:
            skipped.append(stem)
            print("SKIP %-34s %s: %s" % (stem, EXPECT_SKIP[stem]["tag"],
                                          EXPECT_SKIP[stem]["why"]))
            continue
        text, err = emit(stem, src)
        if err:
            print("FAIL %-34s emit: %s" % (stem, err.strip().split("\n")[-1]))
            failed += 1
            continue
        emitted += 1

        exe, err = compile_and_link(stem)
        if err:
            print("FAIL %-34s compile: %s" % (stem, err.strip().split("\n")[-1]))
            failed += 1
            continue

        want_out = open(os.path.join(CORPUS, stem + ".out")).read()
        want_exit = open(os.path.join(CORPUS, stem + ".exit")).read().strip()
        p = subprocess.run([exe], stdout=subprocess.PIPE,
                           stderr=subprocess.STDOUT, cwd=REPO)
        got_out = p.stdout.decode("utf-8", "replace")
        got_exit = str(p.returncode)

        if stem in EXPECT_DIVERGE:
            exp = EXPECT_DIVERGE[stem]
            if got_out == want_out:
                print("FAIL %-34s matched the HOST; must diverge" % stem)
                failed += 1
            elif got_out == exp["out"] and got_exit == exp["exit"]:
                print("OK   %-34s diverged in the frozen way (exit %s)"
                      % (stem, got_exit))
                passed += 1
            else:
                print("FAIL %-34s diverged, but not the frozen way" % stem)
                print("       want %r exit %s" % (exp["out"], exp["exit"]))
                print("       got  %r exit %s" % (got_out, got_exit))
                failed += 1
            continue

        if got_out == want_out and got_exit == want_exit:
            print("OK   %-34s (exit %s)" % (stem, got_exit))
            passed += 1
        else:
            print("FAIL %-34s" % stem)
            if got_exit != want_exit:
                print("       exit: want %s got %s" % (want_exit, got_exit))
            if got_out != want_out:
                print("       want: %r" % want_out)
                print("       got:  %r" % got_out)
            failed += 1

    total = len(list(cases()))
    print("")
    if skipped:
        print("       %d SKIPPED (documented limitations): %s"
              % (len(skipped), ", ".join("%s [%s]" % (s_, EXPECT_SKIP[s_]["tag"])
                                         for s_ in skipped)))
    # The denominator EXCLUDES the skipped cases: a documented limitation is not
    # a case that "did not reach the compiler", and counting it as one would turn
    # a recorded gap into a failure.
    want_emitted = total - len(skipped)
    if emitted != want_emitted:
        print("B-2 GATE FAILED - only %d of %d cases reached the compiler"
              % (emitted, want_emitted))
        return 1
    if skipped:
        print("       %d SKIPPED (documented limitations): %s"
              % (len(skipped), ", ".join("%s [%s]" % (s_, EXPECT_SKIP[s_]["tag"])
                                         for s_ in skipped)))
    if failed == 0:
        print("B-2 GATE OK - %d/%d through the COMPILED path, "
              "emitted code zero warnings" % (passed, total))
        return 0
    print("B-2 GATE FAILED - %d failure(s), %d of %d passed"
          % (failed, passed, total))
    return 1


if __name__ == "__main__":
    sys.exit(main())