# Copyright (C) 2026-2027 Ziad Mohsin. All Rights Reserved.
import re

TOKEN_SPEC = [
    ("MOUNTX",    r"\bmountx\b"),
    ("UNMOUNTX",  r"\bunmountx\b"),
    ("PRINTX",    r"\bprintx\b"),
    ("INPUTX",    r"\binputx\b"),
    ("IMPORTX",   r"\bimportx\b"),
    ("READX",     r"\breadx\b"),
    ("WRITEX",    r"\bwritex\b"),
    ("APPENDX",   r"\bappendx\b"),
    ("EXPORTX",   r"\bexportx\b"),
    ("TRYX",      r"\btryx\b"),
    ("CATCHX",    r"\bcatchx\b"),
    ("THROWX",    r"\b(?:throwx|throw)\b"),
    ("TYPEOF",    r"\b(?:typeofx|typeof|typex)\b"),
    ("NULL",      r"\b(?:nullx|null|nil)\b"),
    ("CLASSX",    r"\b(?:classx|class)\b"),
    ("NEWX",      r"\b(?:newx|new)\b"),
    ("THISX",     r"\b(?:thisx|this|selfx|self)\b"),
    ("SUPER",     r"\bsuper\b"),
    ("EXTENDS",   r"\bextends\b"),
    ("LET",       r"\blet\b"),
    ("FUNC",      r"\bfunc\b"),
    ("RETURN",    r"\breturn\b"),
    ("IF",        r"\bif\b"),
    ("ELSE",      r"\belse\b"),
    ("LOOP",      r"\bloop\b"),
    ("WHILE",     r"\bwhile\b"),
    ("FOR",       r"\bfor\b"),
    ("IN",        r"\bin\b"),
    ("BREAK",     r"\bbreak\b"),
    ("CONTINUE",  r"\bcontinue\b"),
    ("LEN",       r"\b(?:lenx|len|length)\b"),
    ("AND",       r"&&|\band\b"),
    ("OR",        r"\|\||\bor\b"),
    ("NOT",       r"!(?!=)|\bnot\b"),
    ("BOOL",      r"\b(?:true|false)\b"),
    # Must be BEFORE FLOAT so "1.5y" is caught, not lexed as 1.5 + y
    ("BAD_NUM_SUFFIX", r"\d+(?:\.\d*)?[a-zA-Z_]\w*"),
    ("FLOAT",     r"\d+\.\d+"),
    ("BAD_FLOAT", r"\d+\.(?![0-9])"),
    ("INT",       r"\d+"),
    # [^"\\\n] excludes newline -> unterminated strings stop at end of line
    ("STRING",             r'"(?:\\[\\"/bfnrt]|\\u[0-9a-fA-F]{4}|[^"\\\n])*"'),
    ("UNTERMINATED_STRING", r'"(?:\\.|[^"\\\n])*'),
    ("EQEQ",      r"=="),
    ("NEQ",       r"!="),
    ("GTE",       r">="),
    ("LTE",       r"<="),
    ("GT",        r">"),
    ("LT",        r"<"),
    ("ASSIGN",    r"="),
    ("PLUS",      r"\+"),
    ("MINUS",     r"-"),
    ("MUL",       r"\*"),
    ("DIV",       r"/"),
    ("MOD",       r"%"),
    ("LPAREN",    r"\("),
    ("RPAREN",    r"\)"),
    ("LBRACE",    r"\{"),
    ("RBRACE",    r"\}"),
    ("LBRACK",    r"\["),
    ("RBRACK",    r"\]"),
    ("COLON",     r":"),
    ("DOT",       r"\."),
    ("COMMA",     r","),
    ("COMMENT",   r"#[^\n]*"),
    ("SKIP",      r"[ \t\r\n]+"),
    ("IDENT",     r"[a-zA-Z_]\w*"),
    ("MISMATCH",  r"."),
]

TOKEN_REGEX = re.compile("|".join(f"(?P<{name}>{pattern})" for name, pattern in TOKEN_SPEC))


class Token:
    def __init__(self, type_, value=None, line=1, column=1):
        self.type = type_
        self.value = value
        self.line = line
        self.column = column

    def __repr__(self):
        return f"Token({self.type}, {self.value!r}, line={self.line}, col={self.column})"


_ESCAPES = {"\\": "\\", '"': '"', "/": "/",
            "b": "\b", "f": "\f", "n": "\n", "r": "\r", "t": "\t"}
_HEX_DIGITS = set("0123456789abcdefABCDEF")


def decode_string(value):
    content = value[1:-1]
    out, i = [], 0
    while i < len(content):
        c = content[i]
        if c != "\\":
            out.append(c)
            i += 1
            continue
        if i + 1 >= len(content):
            raise SyntaxError(f"Invalid string escape: lone '\\' at end of {value!r}")
        esc = content[i + 1]
        if esc == "u":
            hexpart = content[i + 2:i + 6]
            if len(hexpart) < 4 or any(ch not in _HEX_DIGITS for ch in hexpart):
                raise SyntaxError(f"Invalid string escape: \\u must be followed by 4 hex digits in {value!r}")
            out.append(chr(int(hexpart, 16)))
            i += 6
        elif esc in _ESCAPES:
            out.append(_ESCAPES[esc])
            i += 2
        else:
            raise SyntaxError(f"Invalid string escape '\\{esc}' in {value!r}")
    return "".join(out)


def _bad_escape_reason(fragment):
    content = fragment[1:]
    i = 0
    while i < len(content):
        if content[i] != "\\":
            i += 1
            continue
        if i + 1 >= len(content):
            return "ends with a lone '\\'"
        esc = content[i + 1]
        if esc == "u":
            hexpart = content[i + 2:i + 6]
            if len(hexpart) < 4 or any(ch not in _HEX_DIGITS for ch in hexpart):
                return "'\\u' must be followed by 4 hex digits"
            i += 6
        elif esc in _ESCAPES:
            i += 2
        else:
            return f"invalid escape '\\{esc}'"
    return None


def lex(code: str):
    if not code or not code.strip():
        return [Token("EOF", None, 1, 1)]
    tokens = []
    for match in TOKEN_REGEX.finditer(code):
        kind = match.lastgroup
        value = match.group()
        start = match.start()
        line = code.count("\n", 0, start) + 1
        prev = code.rfind("\n", 0, start)
        col = start + 1 if prev == -1 else start - prev
        if kind in ("SKIP", "COMMENT"):
            continue
        if kind == "BAD_FLOAT":
            raise SyntaxError(f"[Lexer] Line {line}, col {col}: Invalid float '{value}' - trailing dot. Use '{value.rstrip('.')}' or '{value}0'")
        if kind == "BAD_NUM_SUFFIX":
            num = re.match(r"\d+(?:\.\d+)?", value).group()
            rest = value[len(num):]
            raise SyntaxError(f"[Lexer] Line {line}, col {col}: Invalid number '{value}' - a number cannot be followed by letters. Write '{num}' and '{rest}' separately")
        if kind == "UNTERMINATED_STRING":
            reason = _bad_escape_reason(value)
            if reason is not None:
                raise SyntaxError(f"[Lexer] Line {line}, col {col}: Invalid string literal - {reason}")
            raise SyntaxError(f"[Lexer] Line {line}, col {col}: Unterminated string literal (missing closing \" before end of line)")
        if kind == "MISMATCH":
            raise SyntaxError(f"[Lexer] Line {line}, col {col}: Unexpected character {value!r}")
        if kind == "STRING":
            value = decode_string(value)
        elif kind == "INT":
            value = int(value)
        elif kind == "FLOAT":
            value = float(value)
        elif kind == "BOOL":
            value = value == "true"
        tokens.append(Token(kind, value, line, col))
    eof_line = code.count("\n") + 1
    eof_col = len(code.rsplit("\n", 1)[-1]) + 1
    tokens.append(Token("EOF", None, eof_line, eof_col))
    return tokens


def tokenize(code):
    return lex(code)