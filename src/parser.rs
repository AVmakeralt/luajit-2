//! Recursive-descent parser for Lua 5.4 syntax.

use crate::ast::*;
use crate::lexer::{Lexer, Token, TokenKind};

pub struct Parser<'a> {
    lx: Lexer<'a>,
    source_name: String,
    pub last_error: Option<String>,
}

impl<'a> Parser<'a> {
    pub fn new(src: &'a str, source_name: impl Into<String>) -> Self {
        Parser {
            lx: Lexer::new(src),
            source_name: source_name.into(),
            last_error: None,
        }
    }

    fn err(&mut self, msg: impl Into<String>) {
        let line = self.lx.peek().line;
        let msg = format!("{}:{}: {}", self.source_name, line, msg.into());
        self.last_error = Some(msg);
    }

    fn cur(&self) -> &Token {
        self.lx.peek()
    }

    fn cur_kind(&self) -> &TokenKind {
        &self.lx.peek().kind
    }

    fn accept(&mut self, k: &TokenKind) -> bool {
        if std::mem::discriminant(self.cur_kind()) == std::mem::discriminant(k) {
            self.lx.next_token();
            true
        } else {
            false
        }
    }

    fn expect(&mut self, k: &TokenKind, what: &str) {
        if !self.accept(k) {
            self.err(format!("expected {}", what));
        }
    }

    fn line(&self) -> usize {
        self.cur().line
    }

    pub fn parse(&mut self) -> Option<Node> {
        let block = self.parse_block();
        if let Some(ref e) = self.last_error {
            return None;
        }
        if !matches!(self.cur_kind(), TokenKind::Eof) {
            self.err(format!("unexpected trailing input near {}", self.cur_kind()));
            return None;
        }
        let (stmts, _) = block_destruct(block?);
        Some(Node::Chunk(1, stmts))
    }

    fn parse_block(&mut self) -> Option<Node> {
        let line = self.line();
        let mut stmts = Vec::new();

        loop {
            match self.cur_kind() {
                TokenKind::End | TokenKind::Else | TokenKind::Elseif |
                TokenKind::Until | TokenKind::Eof => break,
                TokenKind::Semi => { self.lx.next_token(); continue; }
                TokenKind::Return => {
                    let rl = self.line();
                    self.lx.next_token();
                    let mut vals = Vec::new();
                    let k = self.cur_kind();
                    if !matches!(k, TokenKind::End | TokenKind::Else | TokenKind::Elseif |
                                    TokenKind::Until | TokenKind::Eof | TokenKind::Semi) {
                        loop {
                            if let Some(e) = self.parse_expr() {
                                vals.push(e);
                            } else { return None; }
                            if !self.accept(&TokenKind::Comma) { break; }
                        }
                    }
                    self.accept(&TokenKind::Semi);
                    stmts.push(Node::Return(rl, vals));
                    break;
                }
                _ => {
                    if let Some(st) = self.parse_statement() {
                        stmts.push(st);
                    } else {
                        return None;
                    }
                }
            }
        }

        Some(Node::Do(line, stmts))
    }

    fn parse_statement(&mut self) -> Option<Node> {
        let line = self.line();
        match self.cur_kind().clone() {
            TokenKind::Break => {
                self.lx.next_token();
                Some(Node::Break(line))
            }
            TokenKind::Goto => {
                self.lx.next_token();
                if let TokenKind::Name(n) = self.cur_kind().clone() {
                    self.lx.next_token();
                    Some(Node::Goto(line, n))
                } else {
                    self.err("expected name after 'goto'");
                    None
                }
            }
            TokenKind::DoubleColon => {
                self.lx.next_token();
                if let TokenKind::Name(n) = self.cur_kind().clone() {
                    self.lx.next_token();
                    self.expect(&TokenKind::DoubleColon, "'::'");
                    Some(Node::Label(line, n))
                } else {
                    self.err("expected name after '::'");
                    None
                }
            }
            TokenKind::Do => {
                self.lx.next_token();
                let block = self.parse_block()?;
                self.expect(&TokenKind::End, "'end'");
                Some(block)
            }
            TokenKind::While => {
                self.lx.next_token();
                let cond = self.parse_expr()?;
                self.expect(&TokenKind::Do, "'do'");
                let block = self.parse_block()?;
                self.expect(&TokenKind::End, "'end'");
                let (stmts, _) = block_destruct(block);
                Some(Node::While(line, Box::new(cond), stmts))
            }
            TokenKind::Repeat => {
                self.lx.next_token();
                let block = self.parse_block()?;
                self.expect(&TokenKind::Until, "'until'");
                let cond = self.parse_expr()?;
                let (stmts, _) = block_destruct(block);
                Some(Node::Repeat(line, stmts, Box::new(cond)))
            }
            TokenKind::If => {
                self.lx.next_token();
                let mut branches = Vec::new();
                let mut else_body = None;
                loop {
                    let cond = self.parse_expr()?;
                    self.expect(&TokenKind::Then, "'then'");
                    let block = self.parse_block()?;
                    let (stmts, _) = block_destruct(block);
                    branches.push((cond, stmts));
                    if self.accept(&TokenKind::Elseif) { continue; }
                    break;
                }
                if self.accept(&TokenKind::Else) {
                    let block = self.parse_block()?;
                    let (stmts, _) = block_destruct(block);
                    else_body = Some(stmts);
                }
                self.expect(&TokenKind::End, "'end'");
                Some(Node::If(line, branches, else_body))
            }
            TokenKind::For => {
                self.lx.next_token();
                let first_name = match self.cur_kind().clone() {
                    TokenKind::Name(n) => { self.lx.next_token(); n }
                    _ => { self.err("expected name after 'for'"); return None; }
                };
                if self.accept(&TokenKind::Assign) {
                    // numeric for
                    let init = self.parse_expr()?;
                    self.expect(&TokenKind::Comma, "','");
                    let limit = self.parse_expr()?;
                    let step = if self.accept(&TokenKind::Comma) {
                        Some(Box::new(self.parse_expr()?))
                    } else { None };
                    self.expect(&TokenKind::Do, "'do'");
                    let block = self.parse_block()?;
                    self.expect(&TokenKind::End, "'end'");
                    let (stmts, _) = block_destruct(block);
                    Some(Node::ForNum(line, first_name, Box::new(init), Box::new(limit), step, stmts))
                } else {
                    // generic for
                    let mut names = vec![first_name];
                    while self.accept(&TokenKind::Comma) {
                        match self.cur_kind().clone() {
                            TokenKind::Name(n) => { self.lx.next_token(); names.push(n); }
                            _ => { self.err("expected name in for-list"); return None; }
                        }
                    }
                    self.expect(&TokenKind::In, "'in'");
                    let mut exprs = Vec::new();
                    loop {
                        exprs.push(self.parse_expr()?);
                        if !self.accept(&TokenKind::Comma) { break; }
                    }
                    self.expect(&TokenKind::Do, "'do'");
                    let block = self.parse_block()?;
                    self.expect(&TokenKind::End, "'end'");
                    let (stmts, _) = block_destruct(block);
                    Some(Node::ForIn(line, names, exprs, stmts))
                }
            }
            TokenKind::Function => {
                self.lx.next_token();
                let name = match self.cur_kind().clone() {
                    TokenKind::Name(n) => { self.lx.next_token(); n }
                    _ => { self.err("expected function name"); return None; }
                };
                let proto = self.parse_function_body(&name)?;
                let fnode = Node::Function(line, Box::new(proto));
                // function name() ... end  →  name = function() ... end
                let target = Node::Name(line, name.clone());
                Some(Node::Assign(line, vec![target], vec![fnode]))
            }
            TokenKind::Local => {
                self.lx.next_token();
                if self.accept(&TokenKind::Function) {
                    let fname = match self.cur_kind().clone() {
                        TokenKind::Name(n) => { self.lx.next_token(); n }
                        _ => { self.err("expected name after 'local function'"); return None; }
                    };
                    let proto = self.parse_function_body(&fname)?;
                    let fnode = Node::Function(line, Box::new(proto));
                    Some(Node::Local(line, vec![fname], vec![fnode]))
                } else {
                    let mut names = Vec::new();
                    loop {
                        match self.cur_kind().clone() {
                            TokenKind::Name(n) => { self.lx.next_token(); names.push(n); }
                            _ => { self.err("expected name in local"); return None; }
                        }
                        // skip attributes <const>, <close>
                        if self.accept(&TokenKind::Less) {
                            if matches!(self.cur_kind(), TokenKind::Name(_)) {
                                self.lx.next_token();
                            }
                            self.expect(&TokenKind::Greater, "'>'");
                        }
                        if !self.accept(&TokenKind::Comma) { break; }
                    }
                    let vals = if self.accept(&TokenKind::Assign) {
                        let mut v = Vec::new();
                        loop {
                            v.push(self.parse_expr()?);
                            if !self.accept(&TokenKind::Comma) { break; }
                        }
                        v
                    } else { Vec::new() };
                    Some(Node::Local(line, names, vals))
                }
            }
            _ => {
                // expression statement: call or assignment
                let first = self.parse_suffixed_expr()?;
                if matches!(self.cur_kind(), TokenKind::Assign | TokenKind::Comma) {
                    // assignment
                    let mut targets = vec![first];
                    while self.accept(&TokenKind::Comma) {
                        targets.push(self.parse_suffixed_expr()?);
                    }
                    self.expect(&TokenKind::Assign, "'='");
                    let mut vals = Vec::new();
                    loop {
                        vals.push(self.parse_expr()?);
                        if !self.accept(&TokenKind::Comma) { break; }
                    }
                    Some(Node::Assign(line, targets, vals))
                } else if matches!(first, Node::Call(_, _, _) | Node::MethodCall(_, _, _, _)) {
                    Some(Node::CallStmt(line, Box::new(first)))
                } else {
                    self.err("syntax error: expected statement");
                    None
                }
            }
        }
    }

    fn parse_function_body(&mut self, name: &str) -> Option<FuncProto> {
        self.expect(&TokenKind::LParen, "'('");
        let mut params = Vec::new();
        let mut is_vararg = false;
        if !matches!(self.cur_kind(), TokenKind::RParen) {
            if self.accept(&TokenKind::Dots) {
                is_vararg = true;
            } else {
                loop {
                    match self.cur_kind().clone() {
                        TokenKind::Name(n) => { self.lx.next_token(); params.push(n); }
                        _ => { self.err("expected parameter name"); return None; }
                    }
                    if self.accept(&TokenKind::Comma) {
                        if self.accept(&TokenKind::Dots) {
                            is_vararg = true;
                            break;
                        }
                        continue;
                    }
                    break;
                }
            }
        }
        self.expect(&TokenKind::RParen, "')'");
        let body_line = self.line();
        let block = self.parse_block()?;
        self.expect(&TokenKind::End, "'end'");
        let (stmts, _) = block_destruct(block);
        Some(FuncProto {
            name: Some(name.to_string()),
            params,
            is_vararg,
            body: stmts,
            line: body_line,
        })
    }

    fn parse_table_constructor(&mut self) -> Option<Node> {
        let line = self.line();
        self.lx.next_token(); // consume {
        let mut entries = Vec::new();
        while !matches!(self.cur_kind(), TokenKind::RBrace | TokenKind::Eof) {
            match self.cur_kind() {
                TokenKind::LBracket => {
                    self.lx.next_token();
                    let key = self.parse_expr()?;
                    self.expect(&TokenKind::RBracket, "']'");
                    self.expect(&TokenKind::Assign, "'='");
                    let val = self.parse_expr()?;
                    entries.push(TableEntry::Keyed(key, val));
                }
                TokenKind::Name(_) => {
                    // Could be `name = val` or `name` (positional)
                    let save = self.lx.cur.clone();
                    self.lx.next_token();
                    if matches!(self.cur_kind(), TokenKind::Assign) {
                        let n = if let TokenKind::Name(n) = &save.kind { n.clone() } else { unreachable!() };
                        self.lx.next_token();
                        let val = self.parse_expr()?;
                        entries.push(TableEntry::Named(n, val));
                    } else {
                        // positional: rewind by restoring the token
                        self.lx.cur = save;
                        let val = self.parse_expr()?;
                        entries.push(TableEntry::Positional(val));
                    }
                }
                _ => {
                    let val = self.parse_expr()?;
                    entries.push(TableEntry::Positional(val));
                }
            }
            if !self.accept(&TokenKind::Comma) && !self.accept(&TokenKind::Semi) {
                break;
            }
        }
        self.expect(&TokenKind::RBrace, "'}'");
        Some(Node::Table(line, entries))
    }

    fn parse_primary_expr(&mut self) -> Option<Node> {
        let line = self.line();
        match self.cur_kind().clone() {
            TokenKind::Nil => { self.lx.next_token(); Some(Node::Nil(line)) }
            TokenKind::True => { self.lx.next_token(); Some(Node::True(line)) }
            TokenKind::False => { self.lx.next_token(); Some(Node::False(line)) }
            TokenKind::Integer(n) => { self.lx.next_token(); Some(Node::Int(line, n)) }
            TokenKind::Number(n) => { self.lx.next_token(); Some(Node::Float(line, n)) }
            TokenKind::String(s) => { self.lx.next_token(); Some(Node::Str(line, s)) }
            TokenKind::Dots => { self.lx.next_token(); Some(Node::Dots(line)) }
            TokenKind::Name(n) => { self.lx.next_token(); Some(Node::Name(line, n)) }
            TokenKind::LParen => {
                self.lx.next_token();
                let e = self.parse_expr()?;
                self.expect(&TokenKind::RParen, "')'");
                Some(e)
            }
            TokenKind::LBrace => self.parse_table_constructor(),
            TokenKind::Function => {
                self.lx.next_token();
                let proto = self.parse_function_body("")?;
                Some(Node::Function(line, Box::new(proto)))
            }
            _ => {
                self.err(format!("unexpected symbol {}", self.cur_kind()));
                None
            }
        }
    }

    fn parse_suffixed_expr(&mut self) -> Option<Node> {
        let mut e = self.parse_primary_expr()?;
        loop {
            let line = self.line();
            match self.cur_kind() {
                TokenKind::Dot => {
                    self.lx.next_token();
                    match self.cur_kind().clone() {
                        TokenKind::Name(n) => { self.lx.next_token(); e = Node::Field(line, Box::new(e), n); }
                        _ => { self.err("expected name after '.'"); return None; }
                    }
                }
                TokenKind::LBracket => {
                    self.lx.next_token();
                    let idx = self.parse_expr()?;
                    self.expect(&TokenKind::RBracket, "']'");
                    e = Node::Index(line, Box::new(e), Box::new(idx));
                }
                TokenKind::Colon => {
                    self.lx.next_token();
                    let mname = match self.cur_kind().clone() {
                        TokenKind::Name(n) => { self.lx.next_token(); n }
                        _ => { self.err("expected method name"); return None; }
                    };
                    let args = self.parse_args()?;
                    e = Node::MethodCall(line, Box::new(e), mname, args);
                }
                TokenKind::LParen | TokenKind::LBrace | TokenKind::String(_) => {
                    let args = self.parse_args()?;
                    e = Node::Call(line, Box::new(e), args);
                }
                _ => break,
            }
        }
        Some(e)
    }

    fn parse_args(&mut self) -> Option<Vec<Node>> {
        match self.cur_kind() {
            TokenKind::LParen => {
                self.lx.next_token();
                let mut args = Vec::new();
                if !matches!(self.cur_kind(), TokenKind::RParen) {
                    loop {
                        args.push(self.parse_expr()?);
                        if !self.accept(&TokenKind::Comma) { break; }
                    }
                }
                self.expect(&TokenKind::RParen, "')'");
                Some(args)
            }
            TokenKind::LBrace => {
                let tbl = self.parse_table_constructor()?;
                Some(vec![tbl])
            }
            TokenKind::String(_) => {
                if let Some(s) = self.parse_primary_expr() {
                    Some(vec![s])
                } else { None }
            }
            _ => {
                self.err("expected function arguments");
                None
            }
        }
    }

    fn parse_unop(&mut self) -> Option<Node> {
        let line = self.line();
        let op = match self.cur_kind() {
            TokenKind::Minus => Some(UnOp::Neg),
            TokenKind::Not => Some(UnOp::Not),
            TokenKind::Hash => Some(UnOp::Len),
            TokenKind::Tilde => Some(UnOp::BNot),
            _ => None,
        };
        if let Some(op) = op {
            self.lx.next_token();
            let operand = self.parse_unop()?;
            Some(Node::UnOp(line, op, Box::new(operand)))
        } else {
            self.parse_suffixed_expr()
        }
    }

    fn parse_subexpr(&mut self, limit: i32) -> Option<Node> {
        let mut left = self.parse_unop()?;
        loop {
            let (op, prec, right_assoc) = match self.cur_kind() {
                TokenKind::Or => (BinOp::Or, 1, false),
                TokenKind::And => (BinOp::And, 2, false),
                TokenKind::Less => (BinOp::Lt, 3, false),
                TokenKind::Greater => (BinOp::Gt, 3, false),
                TokenKind::LessEqual => (BinOp::Le, 3, false),
                TokenKind::GreaterEqual => (BinOp::Ge, 3, false),
                TokenKind::EqualEqual => (BinOp::Eq, 3, false),
                TokenKind::NotEqual => (BinOp::Ne, 3, false),
                TokenKind::Pipe => (BinOp::BOr, 4, false),
                TokenKind::Tilde => (BinOp::BXor, 5, false),
                TokenKind::Amp => (BinOp::BAnd, 6, false),
                TokenKind::DoubleLt => (BinOp::Shl, 7, false),
                TokenKind::DoubleGt => (BinOp::Shr, 7, false),
                TokenKind::DoubleSlash => (BinOp::IDiv, 8, false),
                TokenKind::Plus => (BinOp::Add, 9, false),
                TokenKind::Minus => (BinOp::Sub, 9, false),
                TokenKind::Star => (BinOp::Mul, 10, false),
                TokenKind::Slash => (BinOp::Div, 10, false),
                TokenKind::Percent => (BinOp::Mod, 10, false),
                TokenKind::Concat => (BinOp::Concat, 11, true),
                TokenKind::Caret => (BinOp::Pow, 12, true),
                _ => break,
            };
            if prec < limit { break; }
            let line = self.line();
            self.lx.next_token();
            let next_limit = if right_assoc { prec } else { prec + 1 };
            let right = self.parse_subexpr(next_limit)?;
            left = Node::BinOp(line, op, Box::new(left), Box::new(right));
        }
        Some(left)
    }

    fn parse_expr(&mut self) -> Option<Node> {
        self.parse_subexpr(0)
    }
}

/// Destructure a Do node into its statement list and line.
fn block_destruct(n: Node) -> (Vec<Node>, usize) {
    let line = n.line();
    if let Node::Do(line, stmts) = n {
        (stmts, line)
    } else {
        (vec![n], line)
    }
}
