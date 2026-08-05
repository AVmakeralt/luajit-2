//! # LuaVortex
//!
//! A Lua 5.4 frontend for the VORTEX JIT runtime, written in Rust.
//!
//! LuaVortex compiles Lua source code to VORTEX bytecode and runs it on
//! the VORTEX multi-tier JIT runtime. All Lua code is compiled — there
//! is no tree-walking interpreter.
//!
//! ## Architecture
//!
//! ```text
//! Lua source → Lexer → Parser → AST → Codegen → VORTEX bytecode → Runtime
//! ```
//!
//! - **Lexer**: Lua 5.4 tokenizer
//! - **Parser**: recursive-descent parser producing an AST
//! - **Codegen**: walks the AST and emits VORTEX bytecode instructions
//! - **Runtime**: wraps the VORTEX runtime, manages Lua heap objects
//!   (strings, tables, functions), and dispatches extended
//!   `CALL_RUNTIME` opcodes to Rust-implemented stdlib functions
//!
//! ## Example
//!
//! ```no_run
//! use luavortex::{Runtime, Source};
//!
//! let mut rt = Runtime::new().expect("runtime create");
//! rt.run_source("print('Hello, World!')").expect("run");
//! ```

pub mod lexer;
pub mod ast;
pub mod parser;
pub mod value;
pub mod codegen;
pub mod runtime;
pub mod stdlib;

pub use runtime::Runtime;
pub use value::{Value, NULL, UNDEFINED, TRUE, FALSE};

/// A source file with a name (for error messages).
#[derive(Debug, Clone)]
pub struct Source {
    pub name: String,
    pub code: String,
}

impl Source {
    pub fn new(name: impl Into<String>, code: impl Into<String>) -> Self {
        Source { name: name.into(), code: code.into() }
    }

    pub fn from_file(path: impl AsRef<std::path::Path>) -> std::io::Result<Self> {
        let path = path.as_ref();
        let code = std::fs::read_to_string(path)?;
        let name = path.to_string_lossy().to_string();
        Ok(Source { name, code })
    }
}

/// Errors produced by LuaVortex.
#[derive(Debug, Clone)]
pub enum Error {
    Parse(String),
    Compile(String),
    Runtime(String),
}

impl std::fmt::Display for Error {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Error::Parse(msg) => write!(f, "parse error: {}", msg),
            Error::Compile(msg) => write!(f, "compile error: {}", msg),
            Error::Runtime(msg) => write!(f, "runtime error: {}", msg),
        }
    }
}

impl std::error::Error for Error {}

pub type Result<T> = std::result::Result<T, Error>;
