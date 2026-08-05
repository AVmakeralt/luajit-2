//! Lua 5.4 tokenizer.

use std::fmt;

#[derive(Debug, Clone, PartialEq)]
pub enum TokenKind {
    Eof,
    // Literals
    Nil, True, False,
    Number(f64),     // all numbers are f64; is_integer tracks int-ness
    Integer(i64),
    String(Vec<u8>),
    Name(String),
    Dots,            // ...
    // Operators
    Plus, Minus, Star, Slash, DoubleSlash, Percent, Caret, Hash,
    Amp, Pipe, Tilde, DoubleLt, DoubleGt,
    EqualEqual, NotEqual, LessEqual, GreaterEqual, Less, Greater,
    Assign, LParen, RParen, LBrace, RBrace, LBracket, RBracket,
    Semi, Colon, DoubleColon, Comma, Dot, Concat,
    // Keywords
    And, Break, Do, Else, Elseif, End, For, Function, Goto,
    If, In, Local, Not, Or, Repeat, Return, Then, Until, While,
}

#[derive(Debug, Clone)]
pub struct Token {
    pub kind: TokenKind,
    pub line: usize,
    pub col: usize,
}

impl fmt::Display for TokenKind {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            TokenKind::Eof => write!(f, "<eof>"),
            TokenKind::Nil => write!(f, "nil"),
            TokenKind::True => write!(f, "true"),
            TokenKind::False => write!(f, "false"),
            TokenKind::Number(n) => write!(f, "<number {}>", n),
            TokenKind::Integer(n) => write!(f, "<integer {}>", n),
            TokenKind::String(s) => write!(f, "<string {:?}>", String::from_utf8_lossy(s)),
            TokenKind::Name(n) => write!(f, "<name {}>", n),
            TokenKind::Dots => write!(f, "..."),
            TokenKind::Plus => write!(f, "+"),
            TokenKind::Minus => write!(f, "-"),
            TokenKind::Star => write!(f, "*"),
            TokenKind::Slash => write!(f, "/"),
            TokenKind::DoubleSlash => write!(f, "//"),
            TokenKind::Percent => write!(f, "%"),
            TokenKind::Caret => write!(f, "^"),
            TokenKind::Hash => write!(f, "#"),
            TokenKind::Amp => write!(f, "&"),
            TokenKind::Pipe => write!(f, "|"),
            TokenKind::Tilde => write!(f, "~"),
            TokenKind::DoubleLt => write!(f, "<<"),
            TokenKind::DoubleGt => write!(f, ">>"),
            TokenKind::EqualEqual => write!(f, "=="),
            TokenKind::NotEqual => write!(f, "~="),
            TokenKind::LessEqual => write!(f, "<="),
            TokenKind::GreaterEqual => write!(f, ">="),
            TokenKind::Less => write!(f, "<"),
            TokenKind::Greater => write!(f, ">"),
            TokenKind::Assign => write!(f, "="),
            TokenKind::LParen => write!(f, "("),
            TokenKind::RParen => write!(f, ")"),
            TokenKind::LBrace => write!(f, "{{"),
            TokenKind::RBrace => write!(f, "}}"),
            TokenKind::LBracket => write!(f, "["),
            TokenKind::RBracket => write!(f, "]"),
            TokenKind::Semi => write!(f, ";"),
            TokenKind::Colon => write!(f, ":"),
            TokenKind::DoubleColon => write!(f, "::"),
            TokenKind::Comma => write!(f, ","),
            TokenKind::Dot => write!(f, "."),
            TokenKind::Concat => write!(f, ".."),
            TokenKind::And => write!(f, "and"),
            TokenKind::Break => write!(f, "break"),
            TokenKind::Do => write!(f, "do"),
            TokenKind::Else => write!(f, "else"),
            TokenKind::Elseif => write!(f, "elseif"),
            TokenKind::End => write!(f, "end"),
            TokenKind::For => write!(f, "for"),
            TokenKind::Function => write!(f, "function"),
            TokenKind::Goto => write!(f, "goto"),
            TokenKind::If => write!(f, "if"),
            TokenKind::In => write!(f, "in"),
            TokenKind::Local => write!(f, "local"),
            TokenKind::Not => write!(f, "not"),
            TokenKind::Or => write!(f, "or"),
            TokenKind::Repeat => write!(f, "repeat"),
            TokenKind::Return => write!(f, "return"),
            TokenKind::Then => write!(f, "then"),
            TokenKind::Until => write!(f, "until"),
            TokenKind::While => write!(f, "while"),
        }
    }
}

pub struct Lexer<'a> {
    src: &'a [u8],
    pos: usize,
    line: usize,
    col: usize,
    pub cur: Token,
    pub prev: Token,
}

impl<'a> Lexer<'a> {
    pub fn new(src: &'a str) -> Self {
        let mut lx = Lexer {
            src: src.as_bytes(),
            pos: 0,
            line: 1,
            col: 1,
            cur: Token { kind: TokenKind::Eof, line: 1, col: 1 },
            prev: Token { kind: TokenKind::Eof, line: 1, col: 1 },
        };
        lx.next_token(); // prime the first token
        lx
    }

    pub fn peek(&self) -> &Token {
        &self.cur
    }

    fn peek_byte(&self) -> u8 {
        *self.src.get(self.pos).unwrap_or(&0)
    }

    fn peek_byte_at(&self, offset: usize) -> u8 {
        *self.src.get(self.pos + offset).unwrap_or(&0)
    }

    fn advance(&mut self) -> u8 {
        if self.pos >= self.src.len() {
            return 0;
        }
        let c = self.src[self.pos];
        self.pos += 1;
        if c == b'\n' {
            self.line += 1;
            self.col = 1;
        } else {
            self.col += 1;
        }
        c
    }

    fn skip_ws_and_comments(&mut self) {
        loop {
            let c = self.peek_byte();
            match c {
                b' ' | b'\t' | b'\r' | b'\n' | 0x0c | 0x0b => {
                    self.advance();
                }
                b'-' if self.peek_byte_at(1) == b'-' => {
                    self.advance(); // -
                    self.advance(); // -
                    // Check for long comment --[[ ... ]]
                    if self.peek_byte() == b'[' {
                        let save_pos = self.pos;
                        let save_line = self.line;
                        let save_col = self.col;
                        self.advance(); // [
                        let mut level = 0;
                        while self.peek_byte() == b'=' {
                            self.advance();
                            level += 1;
                        }
                        if self.peek_byte() == b'[' {
                            self.advance();
                            // Read until matching ]=*]
                            let mut close = Vec::new();
                            close.push(b']');
                            for _ in 0..level { close.push(b'='); }
                            close.push(b']');
                            loop {
                                if self.peek_byte() == 0 { return; }
                                if self.peek_byte() == b']' {
                                    let mut match_len = 0;
                                    for (i, &cb) in close.iter().enumerate() {
                                        if self.src.get(self.pos + i) != Some(&cb) {
                                            break;
                                        }
                                        match_len = i + 1;
                                    }
                                    if match_len == close.len() {
                                        for _ in 0..close.len() { self.advance(); }
                                        break;
                                    }
                                }
                                self.advance();
                            }
                            continue;
                        }
                        // Not a long comment — rewind
                        self.pos = save_pos;
                        self.line = save_line;
                        self.col = save_col;
                    }
                    // Line comment
                    while self.peek_byte() != 0 && self.peek_byte() != b'\n' {
                        self.advance();
                    }
                }
                _ => return,
            }
        }
    }

    fn read_identifier(&mut self) -> String {
        let start = self.pos;
        while is_ident_part(self.peek_byte()) {
            self.advance();
        }
        String::from_utf8_lossy(&self.src[start..self.pos]).to_string()
    }

    fn read_number(&mut self) -> TokenKind {
        let start = self.pos;
        let mut is_float = false;
        let mut is_hex = false;

        if self.peek_byte() == b'0' && (self.peek_byte_at(1) == b'x' || self.peek_byte_at(1) == b'X') {
            is_hex = true;
            self.advance(); // 0
            self.advance(); // x
            while is_hex_digit(self.peek_byte()) { self.advance(); }
            if self.peek_byte() == b'.' {
                is_float = true;
                self.advance();
                while is_hex_digit(self.peek_byte()) { self.advance(); }
            }
            if self.peek_byte() == b'p' || self.peek_byte() == b'P' {
                is_float = true;
                self.advance();
                if self.peek_byte() == b'+' || self.peek_byte() == b'-' { self.advance(); }
                while self.peek_byte().is_ascii_digit() { self.advance(); }
            }
        } else {
            while self.peek_byte().is_ascii_digit() { self.advance(); }
            if self.peek_byte() == b'.' {
                is_float = true;
                self.advance();
                while self.peek_byte().is_ascii_digit() { self.advance(); }
            }
            if self.peek_byte() == b'e' || self.peek_byte() == b'E' {
                is_float = true;
                self.advance();
                if self.peek_byte() == b'+' || self.peek_byte() == b'-' { self.advance(); }
                while self.peek_byte().is_ascii_digit() { self.advance(); }
            }
        }

        let s = String::from_utf8_lossy(&self.src[start..self.pos]).to_string();
        if is_float {
            TokenKind::Number(s.parse::<f64>().unwrap_or(0.0))
        } else if is_hex {
            TokenKind::Integer(i64::from_str_radix(s.trim_start_matches("0x").trim_start_matches("0X"), 16).unwrap_or(0))
        } else {
            match s.parse::<i64>() {
                Ok(v) => {
                    if v >= -(1 << 45) && v <= (1 << 45) - 1 {
                        TokenKind::Integer(v)
                    } else {
                        TokenKind::Number(v as f64)
                    }
                }
                Err(_) => TokenKind::Number(s.parse::<f64>().unwrap_or(0.0)),
            }
        }
    }

    fn read_string(&mut self, quote: u8) -> Vec<u8> {
        self.advance(); // opening quote
        let mut buf = Vec::new();
        loop {
            let c = self.peek_byte();
            if c == 0 || c == b'\n' {
                break; // unterminated
            }
            if c == quote {
                self.advance();
                break;
            }
            if c == b'\\' {
                self.advance();
                let e = self.advance();
                match e {
                    b'a' => buf.push(0x07),
                    b'b' => buf.push(0x08),
                    b'f' => buf.push(0x0c),
                    b'n' => buf.push(b'\n'),
                    b'r' => buf.push(b'\r'),
                    b't' => buf.push(b'\t'),
                    b'v' => buf.push(0x0b),
                    b'\\' => buf.push(b'\\'),
                    b'"' => buf.push(b'"'),
                    b'\'' => buf.push(b'\''),
                    b'\n' => buf.push(b'\n'),
                    b'x' => {
                        let mut v = 0u8;
                        for _ in 0..2 {
                            let h = hex_val(self.peek_byte());
                            if h < 0 { break; }
                            v = (v << 4) | (h as u8);
                            self.advance();
                        }
                        buf.push(v);
                    }
                    b'z' => {
                        while matches!(self.peek_byte(), b' ' | b'\t' | b'\n' | b'\r') {
                            self.advance();
                        }
                    }
                    d if d.is_ascii_digit() => {
                        let mut v = (d - b'0') as i32;
                        for _ in 0..2 {
                            let nd = self.peek_byte();
                            if nd.is_ascii_digit() {
                                v = v * 10 + (nd - b'0') as i32;
                                self.advance();
                            } else {
                                break;
                            }
                        }
                        buf.push((v & 0xFF) as u8);
                    }
                    other => buf.push(other),
                }
            } else {
                buf.push(c);
                self.advance();
            }
        }
        buf
    }

    fn read_long_string(&mut self) -> Option<Vec<u8>> {
        // Assumes current byte is the first '['
        self.advance(); // consume first [
        let mut level = 0;
        while self.peek_byte() == b'=' {
            self.advance();
            level += 1;
        }
        if self.peek_byte() != b'[' {
            return None;
        }
        self.advance(); // consume second [

        // Skip first newline
        if self.peek_byte() == b'\n' {
            self.advance();
        }

        let mut close = Vec::new();
        close.push(b']');
        for _ in 0..level { close.push(b'='); }
        close.push(b']');

        let mut buf = Vec::new();
        loop {
            if self.peek_byte() == 0 {
                return None; // unterminated
            }
            if self.peek_byte() == b']' {
                let mut matched = true;
                for (i, &cb) in close.iter().enumerate() {
                    if self.src.get(self.pos + i) != Some(&cb) {
                        matched = false;
                        break;
                    }
                }
                if matched {
                    for _ in 0..close.len() { self.advance(); }
                    break;
                }
            }
            buf.push(self.advance());
        }
        Some(buf)
    }

    pub fn next_token(&mut self) -> &Token {
        self.prev = self.cur.clone();
        self.skip_ws_and_comments();

        let line = self.line;
        let col = self.col;
        let c = self.peek_byte();

        let kind = if c == 0 {
            TokenKind::Eof
        } else if is_ident_start(c) {
            let name = self.read_identifier();
            match name.as_str() {
                "and" => TokenKind::And,
                "break" => TokenKind::Break,
                "do" => TokenKind::Do,
                "else" => TokenKind::Else,
                "elseif" => TokenKind::Elseif,
                "end" => TokenKind::End,
                "for" => TokenKind::For,
                "function" => TokenKind::Function,
                "goto" => TokenKind::Goto,
                "if" => TokenKind::If,
                "in" => TokenKind::In,
                "local" => TokenKind::Local,
                "nil" => TokenKind::Nil,
                "not" => TokenKind::Not,
                "or" => TokenKind::Or,
                "repeat" => TokenKind::Repeat,
                "return" => TokenKind::Return,
                "then" => TokenKind::Then,
                "true" => TokenKind::True,
                "false" => TokenKind::False,
                "until" => TokenKind::Until,
                "while" => TokenKind::While,
                _ => TokenKind::Name(name),
            }
        } else if c.is_ascii_digit() || (c == b'.' && self.peek_byte_at(1).is_ascii_digit()) {
            self.read_number()
        } else if c == b'"' || c == b'\'' {
            TokenKind::String(self.read_string(c))
        } else if c == b'[' {
            let save_pos = self.pos;
            let save_line = self.line;
            let save_col = self.col;
            if let Some(s) = self.read_long_string() {
                TokenKind::String(s)
            } else {
                self.pos = save_pos;
                self.line = save_line;
                self.col = save_col;
                self.advance();
                TokenKind::LBracket
            }
        } else {
            self.advance();
            match c {
                b'+' => TokenKind::Plus,
                b'-' => TokenKind::Minus,
                b'*' => TokenKind::Star,
                b'/' => {
                    if self.peek_byte() == b'/' { self.advance(); TokenKind::DoubleSlash }
                    else { TokenKind::Slash }
                }
                b'%' => TokenKind::Percent,
                b'^' => TokenKind::Caret,
                b'#' => TokenKind::Hash,
                b'&' => TokenKind::Amp,
                b'|' => TokenKind::Pipe,
                b'~' => {
                    if self.peek_byte() == b'=' { self.advance(); TokenKind::NotEqual }
                    else { TokenKind::Tilde }
                }
                b'<' => {
                    if self.peek_byte() == b'<' { self.advance(); TokenKind::DoubleLt }
                    else if self.peek_byte() == b'=' { self.advance(); TokenKind::LessEqual }
                    else { TokenKind::Less }
                }
                b'>' => {
                    if self.peek_byte() == b'>' { self.advance(); TokenKind::DoubleGt }
                    else if self.peek_byte() == b'=' { self.advance(); TokenKind::GreaterEqual }
                    else { TokenKind::Greater }
                }
                b'=' => {
                    if self.peek_byte() == b'=' { self.advance(); TokenKind::EqualEqual }
                    else { TokenKind::Assign }
                }
                b'(' => TokenKind::LParen,
                b')' => TokenKind::RParen,
                b'{' => TokenKind::LBrace,
                b'}' => TokenKind::RBrace,
                b']' => TokenKind::RBracket,
                b';' => TokenKind::Semi,
                b':' => {
                    if self.peek_byte() == b':' { self.advance(); TokenKind::DoubleColon }
                    else { TokenKind::Colon }
                }
                b',' => TokenKind::Comma,
                b'.' => {
                    if self.peek_byte() == b'.' {
                        self.advance();
                        if self.peek_byte() == b'.' { self.advance(); TokenKind::Dots }
                        else { TokenKind::Concat }
                    } else {
                        TokenKind::Dot
                    }
                }
                _ => TokenKind::Eof,
            }
        };

        self.cur = Token { kind, line, col };
        &self.cur
    }
}

fn is_ident_start(c: u8) -> bool {
    c.is_ascii_alphabetic() || c == b'_'
}

fn is_ident_part(c: u8) -> bool {
    c.is_ascii_alphanumeric() || c == b'_'
}

fn is_hex_digit(c: u8) -> bool {
    c.is_ascii_hexdigit()
}

fn hex_val(c: u8) -> i8 {
    match c {
        b'0'..=b'9' => (c - b'0') as i8,
        b'a'..=b'f' => (c - b'a' + 10) as i8,
        b'A'..=b'F' => (c - b'A' + 10) as i8,
        _ => -1,
    }
}
