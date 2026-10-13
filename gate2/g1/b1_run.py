# Copyright (C) 2026-2027 Ziad Mohsin. All Rights Reserved.
#!/usr/bin/env python3
"""b1_run.py - the B-1 gate. The corpus is the deliverable; this checks it.

Runs every case in selfhost/b1/corpus through a built mountx binary (default:
selfhost/b1/mountx) and compares stdout byte-for-byte with the .out golden and
the exit code with the .exit file.

B-0's condition 5: the gate exists before the code. This file therefore runs
against a BINARY THAT DOES NOT EXIST YET, and says so, rather than passing
vacuously. That is the intended state until mini.c lands.

The one deliberate inversion is G2: case 12 is EXPECTED TO DIVERGE, because the
host prints a 19-digit number and the C port must refuse (COMPILE_DESIGN §9.4,
divergence D-C2). A C port that MATCHES the host there has no overflow guard,
which is the bug silent wrapping always is - so a match fails as loudly as a
mismatch.
"""

import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
CORPUS = os.path.join(HERE, "corpus")
REPO = os.path.dirname(os.path.dirname(HERE))

# The frozen C-side answer for the one case that must diverge.
EXPECT_DIVERGE = {
    "12_overflow_refusal": {
        "out": "[Runtime] Integer overflow: result does not fit in 64 bits\n",
        "exit": "1",
    },
}

# KNOWN LIMITATIONS: cases that are NOT failures and NOT passes. Each is a real,
# measured gap in the C port, recorded with the tag that names it - so the
# limitation stays VISIBLE in every gate run instead of being a silence.
#
# A SKIP is not a way to make a gate green. It is a claim, and it is only
# admissible because the gate PRINTS it: "19 passed, 1 skipped (D-ARR1)" says
# something true and checkable, whereas a quietly-dropped case says nothing at
# all. Every entry must name a tag that appears in COMPILE_DESIGN.md.
EXPECT_SKIP = {
    "20_darr1_nonascii_slice": {
        "tag": "D-ARR1",
        "why": ("string indexing returns a 1-character string and mx_len counts "
                "BYTES, so a non-ASCII character is mis-sliced. The corpus is "
                "ASCII, so no case could fail on this; this one is here to be "
                "SKIPPED rather than absent. See COMPILE_DESIGN.md D-ARR1."),
    },
}


# A module file is NOT a case. case 23's helper (mod_helper.mx) sits in the
# corpus directory because that is what makes the import walk's SECOND base -
# the entry script's directory - the thing under test. Distinguishing the two by
# "has a golden" is right: a case always has one, and a module never does.
def cases():
    for name in sorted(os.listdir(CORPUS)):
        if not name.endswith(".mx"):
            continue
        stem = name[:-3]
        if not (os.path.exists(os.path.join(CORPUS, stem + ".out")) or
                stem in EXPECT_SKIP):
            print("NOTE %-34s no golden and not a registered skip - treated as a "
                  "MODULE, not a case" % stem)
            continue
        yield stem, os.path.join(CORPUS, name)


def build():
    """CB4's flags. Zero warnings is gate G3, so a warning fails the gate."""
    # -lm goes LAST: the linker resolves left to right, so a library listed
    # before the object that needs it is discarded, and mini.c needs fmod for
    # float modulo (interp.mx:1128 does `l[1] % r[1]` on numbers).
    flags = ["-std=c99", "-O2", "-Wall", "-Wextra", "-Wclobbered"]
    libs = ["-lm", "-lsqlite3"]
# -lsqlite3 because the db_* natives are REAL as of B-3.5: CB-E ("no
# dependencies beyond libc") was retracted when libsqlite3-dev landed on this
# machine, and db_exec's rowcount -1 for DDL is a shape the corpus asserts. The
# library is a LINK dependency of both gates now, which is a deliberate change
# of CB-E and not an accident - so it is written here rather than discovered by
# a linker error on someone else's box.
    out = []
    for src, exe in (("mini.c", "mountx"), ("pyrepr_test.c", "pyrepr_test")):
        p = subprocess.run(["cc"] + flags + ["-o", exe, src] + libs,
                           capture_output=True, cwd=HERE)
        if p.returncode != 0:
            print("BUILD FAILED for %s" % src)
            print(p.stderr.decode("utf-8", "replace"))
            return False, False
        if p.stderr:
            out.append(p.stderr.decode("utf-8", "replace"))
            print("G3 FAIL - %s produced warnings (target is ZERO):" % src)
            print(out[-1])
            return False, False
    return True, True


def run(binary, path):
    # The goldens were produced by the HOST with 2>&1 merged, because
    # main.py prints an uncaught error on stdout while some hosts print theirs on
    # stderr. mini.c writes uncaught errors to stderr, so the harness must merge
    # too or the comparison measures a stream choice rather than the text.
    p = subprocess.run([binary, path], stdout=subprocess.PIPE,
                       stderr=subprocess.STDOUT, cwd=REPO)
    return p.stdout.decode("utf-8", "replace"), p.returncode


def main():
    binary = sys.argv[1] if len(sys.argv) > 1 else None

    # G3: build with CB4's flags; zero warnings or the gate fails.
    if binary is None:
        ok, _ = build()
        if not ok:
            return 1
        binary = os.path.join(HERE, "mountx")
    elif not os.path.exists(binary):
        print("B-1 GATE: no binary at %s" % binary)
        print("cases in the corpus: %d" % len(list(cases())))
        return 2

    # G4: the float spellings, which cannot be written as .mx literals.
    pt = subprocess.run([os.path.join(HERE, "pyrepr_test")],
                        capture_output=True, cwd=HERE)
    g4_ok = pt.returncode == 0
    print("G4  mx_py_repr unit test: %s" % ("OK" if g4_ok else "FAILED"))
    if not g4_ok:
        print(pt.stdout.decode("utf-8", "replace"))

    passed = failed = 0
    skipped = []
    for stem, path in cases():
        if stem in EXPECT_SKIP:
            # SKIPPED, and REPORTED as skipped. The golden is still compared
            # against the HOST so the .out file cannot rot unnoticed - only the
            # C port's answer is not demanded to match it.
            out_path = os.path.join(CORPUS, stem + ".out")
            exit_path = os.path.join(CORPUS, stem + ".exit")
            if os.path.exists(out_path) and os.path.exists(exit_path):
                # The HOST runs the case, not the C port: this checks that the
                # golden still records what the language does. Using the C binary
                # here would compare the port against its own limitation and call
                # it a host change.
                hp = subprocess.run([sys.executable, os.path.join(REPO, "main.py"),
                                     path],
                                    stdout=subprocess.PIPE,
                                    stderr=subprocess.STDOUT, cwd=REPO,
                                    env=dict(os.environ,
                                             MOUNTX_STD=os.path.join(REPO, "stdlib")))
                want = open(out_path).read()
                if hp.stdout.decode("utf-8", "replace") != want:
                    print("FAIL %-34s DIVERGED from the host golden - the "
                          "limitation is documented but the HOST behaviour "
                          "changed" % stem)
                    failed += 1
                    continue
            skipped.append(stem)
            print("SKIP %-34s %s: %s" % (stem, EXPECT_SKIP[stem]["tag"],
                                          EXPECT_SKIP[stem]["why"]))
            continue
        out_path = os.path.join(CORPUS, stem + ".out")
        exit_path = os.path.join(CORPUS, stem + ".exit")
        if not os.path.exists(out_path) or not os.path.exists(exit_path):
            print("FAIL %-34s no golden (out/exit missing)" % stem)
            failed += 1
            continue

        want_out = open(out_path).read()
        want_exit = open(exit_path).read().strip()
        got_out, got_exit = run(binary, path)
        got_exit = str(got_exit)

        if stem in EXPECT_DIVERGE:
            exp = EXPECT_DIVERGE[stem]
            if got_out == want_out:
                print("FAIL %-34s matched the HOST, but must diverge (no"
                      " overflow guard?)" % stem)
                failed += 1
            elif got_out == exp["out"] and got_exit == exp["exit"]:
                print("OK   %-34s diverged in the frozen way (exit %s)"
                      % (stem, got_exit))
                passed += 1
            else:
                print("FAIL %-34s diverged, but not the frozen way" % stem)
                print("       want: %r exit %s" % (exp["out"], exp["exit"]))
                print("       got:  %r exit %s" % (got_out, got_exit))
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

    print("")
    if failed == 0 and g4_ok:
        print("B-1 GATE OK - %d case(s) passed, G4 py_repr green, G3 zero warnings"
              % passed)
        return 0
    print("B-1 GATE FAILED - %d case failure(s), %d passed, G4 %s"
          % (failed, passed, "OK" if g4_ok else "FAILED"))
    return 1


if __name__ == "__main__":
    sys.exit(main())