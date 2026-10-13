# Copyright (C) 2026-2027 Ziad Mohsin. All Rights Reserved.
from lexer import Token

EXPR_STARTERS = frozenset({
    "INT", "FLOAT", "STRING", "BOOL", "NULL", "MINUS", "NOT",
    "IDENT", "THISX", "SUPER", "LPAREN", "LBRACK",
    "INPUTX", "LEN", "TYPEOF", "NEWX",
    "READX", "WRITEX", "APPENDX", "EXPORTX",
})

FILE_BUILTINS = {
    "READX":   ("Read",   1),   # readx(path)
    "WRITEX":  ("Write",  2),   # writex(path, content)
    "APPENDX": ("Append", 2),   # appendx(path, content)
    "EXPORTX": ("Export", 2),   # exportx(path, value)
}


class Parser:
    def __init__(self, tokens):
        if not tokens:
            tokens = [Token("EOF", None, 1, 1)]
        self.tokens = tokens
        self.pos = 0

    def current(self):
        if self.pos >= len(self.tokens):
            last = self.tokens[-1] if self.tokens else Token("EOF", None, 1, 1)
            return Token("EOF", None, last.line, last.column)
        return self.tokens[self.pos]

    def peek(self, offset=1):
        pos = self.pos + offset
        if pos >= len(self.tokens):
            return self.current()
        return self.tokens[pos]

    def eat(self, expected=None):
        token = self.current()
        if expected is not None and token.type != expected:
            self.error(f"Expected {expected}, got {token.type} ({token.value!r})")
        self.pos += 1
        return token

    def error(self, message):
        token = self.current()
        raise SyntaxError(f"[Parser] Line {token.line}, col {token.column}: {message}")

    def parse(self):
        if self.current().type != "MOUNTX":
            self.error("Program must start with mountx")
        self.eat("MOUNTX")
        body = []
        while self.current().type not in ("UNMOUNTX", "EOF"):
            body.append(self.parse_statement())
        if self.current().type == "EOF":
            self.error("Program is missing unmountx")
        self.eat("UNMOUNTX")
        if self.current().type != "EOF":
            self.error("Unexpected code after unmountx")
        return {"type": "Program", "body": body, "line": 1}

    def parse_statement(self):
        kind = self.current().type
        if kind == "LET":      return self.parse_let()
        if kind == "PRINTX":   return self.parse_print()
        if kind == "IMPORTX":  return self.parse_import()
        if kind == "IF":       return self.parse_if()
        if kind == "WHILE":    return self.parse_while()
        if kind == "LOOP":     return self.parse_loop()
        if kind == "FOR":      return self.parse_for()
        if kind == "FUNC":     return self.parse_function()
        if kind == "CLASSX":   return self.parse_class()
        if kind == "RETURN":   return self.parse_return()
        if kind == "BREAK":
            t = self.eat("BREAK")
            return {"type": "Break", "line": t.line}
        if kind == "CONTINUE":
            t = self.eat("CONTINUE")
            return {"type": "Continue", "line": t.line}
        if kind == "TRYX":     return self.parse_try()
        if kind == "THROWX":   return self.parse_throw()
        if kind in ("IDENT", "THISX", "SUPER"):
            return self.parse_assignment_or_expression()
        if kind in EXPR_STARTERS:
            expr = self.parse_expression()
            return {"type": "ExprStmt", "expr": expr, "line": expr.get("line", 0)}
        self.error(f"Unexpected token {kind} ({self.current().value!r})")

    def parse_let(self):
        tok = self.eat("LET")
        name = self.eat("IDENT").value
        self.eat("ASSIGN")
        val = self.parse_expression()
        return {"type": "Let", "name": name, "value": val, "line": tok.line}

    def parse_print(self):
        tok = self.eat("PRINTX")
        self.eat("LPAREN")
        val = self.parse_expression()
        self.eat("RPAREN")
        return {"type": "Print", "value": val, "line": tok.line}

    def parse_import(self):
        tok = self.eat("IMPORTX")
        name = self.eat("IDENT").value
        return {"type": "Import", "name": name, "line": tok.line}

    def parse_class(self):
        tok = self.eat("CLASSX")
        name = self.eat("IDENT").value
        parent = None
        if self.current().type == "EXTENDS":
            self.eat("EXTENDS")
            # dotted names allowed: extends dict.Dict
            parent = self.eat("IDENT").value
            while self.current().type == "DOT":
                self.eat("DOT")
                parent += "." + self.eat("IDENT").value
        open_tok = self.eat("LBRACE")
        fields, methods, init_node = [], [], None
        while self.current().type != "RBRACE":
            if self.current().type == "EOF":
                self.error(f"Missing closing }} for class '{name}' (opened on line {open_tok.line})")
            if self.current().type == "LET":
                f_tok = self.eat("LET")
                f_name = self.eat("IDENT").value
                self.eat("ASSIGN")
                f_val = self.parse_expression()
                fields.append({"name": f_name, "value": f_val, "line": f_tok.line})
            elif self.current().type == "FUNC":
                func = self.parse_function()
                if func["name"] == "init":
                    if init_node is not None:
                        self.error(f"Duplicate init function in class '{name}'")
                    init_node = func
                else:
                    methods.append(func)
            else:
                self.error(f"Unexpected token {self.current().type} in class body (expected a 'let' field or 'func' method)")
        self.eat("RBRACE")
        return {"type": "ClassDef", "name": name, "parent": parent,
                "fields": fields, "methods": methods, "init": init_node, "line": tok.line}

    def _is_assignable_base(self, node):
        t = node["type"]
        if t in ("Ident", "This", "Super"):
            return True
        if t == "DotAccess":
            return self._is_assignable_base(node["object"])
        if t == "Index":
            return self._is_assignable_base(node["array"])
        return False

    def parse_assignment_or_expression(self):
        expr = self.parse_expression()
        if self.current().type == "ASSIGN":
            if expr["type"] in ("This", "Super"):
                self.error(f"Cannot assign to '{expr['type'].lower()}' - '{expr['type'].lower()} = ...' is invalid")
            if expr["type"] not in ("Ident", "DotAccess", "Index"):
                self.error(f"Invalid assignment target: {expr['type']}")
            if expr["type"] == "DotAccess" and not self._is_assignable_base(expr["object"]):
                self.error("Cannot assign to a property of a temporary value")
            if expr["type"] == "Index" and not self._is_assignable_base(expr["array"]):
                self.error("Cannot assign into a temporary value")
            self.eat("ASSIGN")
            val = self.parse_expression()
            return {"type": "Assign", "target": expr, "value": val, "line": expr.get("line", 0)}
        return {"type": "ExprStmt", "expr": expr, "line": expr.get("line", 0)}

    def parse_if(self):
        tok = self.eat("IF")
        cond = self.parse_expression()
        then_b = self.parse_block()
        else_b = []
        if self.current().type == "ELSE":
            self.eat("ELSE")
            if self.current().type == "IF":
                else_b = [self.parse_if()]
            else:
                else_b = self.parse_block()
        return {"type": "If", "condition": cond, "then": then_b, "else": else_b, "line": tok.line}

    def parse_while(self):
        tok = self.eat("WHILE")
        cond = self.parse_expression()
        body = self.parse_block()
        return {"type": "While", "condition": cond, "body": body, "line": tok.line}

    def parse_loop(self):
        tok = self.eat("LOOP")
        count = self.parse_expression()
        body = self.parse_block()
        return {"type": "Loop", "count": count, "body": body, "line": tok.line}

    def parse_for(self):
        tok = self.eat("FOR")
        var = self.eat("IDENT").value
        var2 = None
        if self.current().type == "COMMA":
            self.eat("COMMA")
            var2 = self.eat("IDENT").value
        self.eat("IN")
        it = self.parse_expression()
        body = self.parse_block()
        return {"type": "ForIn", "var": var, "var2": var2, "iterable": it, "body": body, "line": tok.line}

    def parse_block(self):
        open_tok = self.eat("LBRACE")
        body = []
        while self.current().type not in ("RBRACE", "EOF"):
            body.append(self.parse_statement())
        if self.current().type == "EOF":
            self.error(f"Missing closing }} for block opened on line {open_tok.line}")
        self.eat("RBRACE")
        return body

    def parse_function(self):
        tok = self.eat("FUNC")
        name = self.eat("IDENT").value
        self.eat("LPAREN")
        params = []
        if self.current().type != "RPAREN":
            params.append(self.eat("IDENT").value)
            while self.current().type == "COMMA":
                self.eat("COMMA")
                params.append(self.eat("IDENT").value)
        self.eat("RPAREN")
        body = self.parse_block()
        return {"type": "FuncDef", "name": name, "params": params, "body": body, "line": tok.line}

    def parse_return(self):
        tok = self.eat("RETURN")
        value = self.parse_expression() if self.current().type in EXPR_STARTERS else None
        return {"type": "Return", "value": value, "line": tok.line}

    def parse_try(self):
        tok = self.eat("TRYX")
        try_b = self.parse_block()
        if self.current().type != "CATCHX":
            self.error("tryx must be followed by catchx(...)")
        self.eat("CATCHX")
        self.eat("LPAREN")
        var = self.eat("IDENT").value
        self.eat("RPAREN")
        catch_b = self.parse_block()
        return {"type": "TryCatch", "try_body": try_b, "catch_var": var, "catch_body": catch_b, "line": tok.line}

    def parse_throw(self):
        tok = self.eat("THROWX")
        v = self.parse_expression()
        return {"type": "Throw", "value": v, "line": tok.line}

    # precedence: or < and < comparison < additive < multiplicative < unary < postfix
    def parse_expression(self):
        return self.parse_logical_or()

    def parse_logical_or(self):
        left = self.parse_logical_and()
        while self.current().type == "OR":
            op_tok = self.eat()
            right = self.parse_logical_and()
            left = {"type": "BinOp", "op": op_tok.type, "left": left, "right": right, "line": op_tok.line}
        return left

    def parse_logical_and(self):
        left = self.parse_comparison()
        while self.current().type == "AND":
            op_tok = self.eat()
            right = self.parse_comparison()
            left = {"type": "BinOp", "op": op_tok.type, "left": left, "right": right, "line": op_tok.line}
        return left

    def parse_comparison(self):
        left = self.parse_additive()
        while self.current().type in ("EQEQ", "NEQ", "GT", "LT", "GTE", "LTE"):
            op_tok = self.eat()
            right = self.parse_additive()
            left = {"type": "BinOp", "op": op_tok.type, "left": left, "right": right, "line": op_tok.line}
        return left

    def parse_additive(self):
        left = self.parse_multiplicative()
        while self.current().type in ("PLUS", "MINUS"):
            op_tok = self.eat()
            right = self.parse_multiplicative()
            left = {"type": "BinOp", "op": op_tok.type, "left": left, "right": right, "line": op_tok.line}
        return left

    def parse_multiplicative(self):
        left = self.parse_unary()
        while self.current().type in ("MUL", "DIV", "MOD"):
            op_tok = self.eat()
            right = self.parse_unary()
            left = {"type": "BinOp", "op": op_tok.type, "left": left, "right": right, "line": op_tok.line}
        return left

    def parse_unary(self):
        if self.current().type in ("MINUS", "NOT"):
            tok = self.eat()
            return {"type": "UnaryOp", "op": tok.type, "value": self.parse_unary(), "line": tok.line}
        return self.parse_postfix()

    def parse_postfix(self):
        node = self.parse_primary()
        while True:
            if self.current().type == "LPAREN":
                l_tok = self.eat("LPAREN")
                args = []
                if self.current().type != "RPAREN":
                    args.append(self.parse_expression())
                    while self.current().type == "COMMA":
                        self.eat("COMMA")
                        args.append(self.parse_expression())
                self.eat("RPAREN")
                node = {"type": "Call", "callee": node, "args": args, "line": l_tok.line}
            elif self.current().type == "DOT":
                dot_tok = self.eat("DOT")
                field = self.eat("IDENT")
                node = {"type": "DotAccess", "object": node, "field": field.value, "line": dot_tok.line}
            elif self.current().type == "LBRACK":
                lb_tok = self.eat("LBRACK")
                idx = self.parse_expression()
                self.eat("RBRACK")
                node = {"type": "Index", "array": node, "index": idx, "line": lb_tok.line}
            else:
                break
        return node

    def parse_primary(self):
        tok = self.current()
        if tok.type in ("INT", "FLOAT", "STRING", "BOOL"):
            self.eat()
            return {"type": "Literal", "value": tok.value, "value_type": tok.type, "line": tok.line}
        if tok.type == "NULL":
            self.eat("NULL")
            return {"type": "Literal", "value": None, "value_type": "NULL", "line": tok.line}
        if tok.type == "IDENT":
            self.eat("IDENT")
            return {"type": "Ident", "name": tok.value, "line": tok.line}
        if tok.type == "THISX":
            self.eat("THISX")
            return {"type": "This", "line": tok.line}
        if tok.type == "SUPER":
            self.eat("SUPER")
            return {"type": "Super", "line": tok.line}
        if tok.type == "INPUTX":
            self.eat("INPUTX")
            prompt = None
            if self.current().type == "LPAREN":
                self.eat("LPAREN")
                if self.current().type != "RPAREN":
                    prompt = self.parse_expression()
                self.eat("RPAREN")
            return {"type": "Input", "prompt": prompt, "line": tok.line}
        if tok.type == "LEN":
            self.eat("LEN")
            self.eat("LPAREN")
            v = self.parse_expression()
            self.eat("RPAREN")
            return {"type": "Call",
                    "callee": {"type": "Ident", "name": "len", "line": tok.line},
                    "args": [v], "line": tok.line}
        if tok.type == "TYPEOF":
            self.eat("TYPEOF")
            self.eat("LPAREN")
            v = self.parse_expression()
            self.eat("RPAREN")
            return {"type": "TypeOf", "value": v, "line": tok.line}
        if tok.type == "NEWX":
            self.eat("NEWX")
            # dotted names allowed: newx dict.Dict(...)
            cname = self.eat("IDENT").value
            while self.current().type == "DOT":
                self.eat("DOT")
                cname += "." + self.eat("IDENT").value
            self.eat("LPAREN")
            args = []
            if self.current().type != "RPAREN":
                args.append(self.parse_expression())
                while self.current().type == "COMMA":
                    self.eat("COMMA")
                    args.append(self.parse_expression())
            self.eat("RPAREN")
            return {"type": "New", "class": cname, "args": args, "line": tok.line}
        if tok.type in FILE_BUILTINS:
            node_type, arity = FILE_BUILTINS[tok.type]
            self.eat()
            self.eat("LPAREN")
            args = []
            if self.current().type != "RPAREN":
                args.append(self.parse_expression())
                while self.current().type == "COMMA":
                    self.eat("COMMA")
                    args.append(self.parse_expression())
            self.eat("RPAREN")
            if len(args) != arity:
                self.error(f"{tok.value} expects {arity} argument(s), got {len(args)}")
            return {"type": node_type, "args": args, "line": tok.line}
        if tok.type == "LPAREN":
            self.eat("LPAREN")
            expr = self.parse_expression()
            self.eat("RPAREN")
            return expr
        if tok.type == "LBRACK":
            return self.parse_array()
        self.error(f"Unexpected {tok.type} ({tok.value!r}) inside expression")

    def parse_array(self):
        lb = self.eat("LBRACK")
        elems = []
        if self.current().type != "RBRACK":
            elems.append(self.parse_expression())
            while self.current().type == "COMMA":
                self.eat("COMMA")
                if self.current().type == "RBRACK":
                    break
                elems.append(self.parse_expression())
        self.eat("RBRACK")
        return {"type": "ArrayLiteral", "elements": elems, "line": lb.line}


def parse(tokens):
    return Parser(tokens).parse()