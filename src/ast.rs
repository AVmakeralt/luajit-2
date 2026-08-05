//! Abstract syntax tree node types.

#[derive(Debug, Clone)]
pub enum BinOp {
    Add, Sub, Mul, Div, IDiv, Mod, Pow,
    Concat,
    Eq, Ne, Lt, Le, Gt, Ge,
    And, Or,
    BAnd, BOr, BXor, Shl, Shr,
}

#[derive(Debug, Clone)]
pub enum UnOp {
    Neg, Not, Len, BNot,
}

#[derive(Debug, Clone)]
pub struct FuncProto {
    pub name: Option<String>,
    pub params: Vec<String>,
    pub is_vararg: bool,
    pub body: Vec<Node>,
    pub line: usize,
}

#[derive(Debug, Clone)]
pub enum Node {
    // Literals
    Nil(usize),
    True(usize),
    False(usize),
    Int(usize, i64),
    Float(usize, f64),
    Str(usize, Vec<u8>),
    Dots(usize),

    // Variables
    Name(usize, String),
    Index(usize, Box<Node>, Box<Node>),       // t[k]
    Field(usize, Box<Node>, String),           // t.name

    // Table constructor
    Table(usize, Vec<TableEntry>),

    // Operations
    BinOp(usize, BinOp, Box<Node>, Box<Node>),
    UnOp(usize, UnOp, Box<Node>),
    Call(usize, Box<Node>, Vec<Node>),
    MethodCall(usize, Box<Node>, String, Vec<Node>),

    // Function literal
    Function(usize, Box<FuncProto>),

    // Statements
    Local(usize, Vec<String>, Vec<Node>),
    Assign(usize, Vec<Node>, Vec<Node>),
    CallStmt(usize, Box<Node>),
    Do(usize, Vec<Node>),
    While(usize, Box<Node>, Vec<Node>),
    Repeat(usize, Vec<Node>, Box<Node>),
    If(usize, Vec<(Node, Vec<Node>)>, Option<Vec<Node>>),
    ForNum(usize, String, Box<Node>, Box<Node>, Option<Box<Node>>, Vec<Node>),
    ForIn(usize, Vec<String>, Vec<Node>, Vec<Node>),
    Return(usize, Vec<Node>),
    Break(usize),
    Goto(usize, String),
    Label(usize, String),

    // Chunk (root)
    Chunk(usize, Vec<Node>),
}

#[derive(Debug, Clone)]
pub enum TableEntry {
    Positional(Node),               // {x, y, z}
    Keyed(Node, Node),              // {[k] = v}
    Named(String, Node),            // {name = v}
}

impl Node {
    pub fn line(&self) -> usize {
        match self {
            Node::Nil(l) | Node::True(l) | Node::False(l) | Node::Int(l, _) |
            Node::Float(l, _) | Node::Str(l, _) | Node::Dots(l) |
            Node::Name(l, _) | Node::Index(l, _, _) | Node::Field(l, _, _) |
            Node::Table(l, _) | Node::BinOp(l, _, _, _) | Node::UnOp(l, _, _) |
            Node::Call(l, _, _) | Node::MethodCall(l, _, _, _) |
            Node::Function(l, _) | Node::Local(l, _, _) | Node::Assign(l, _, _) |
            Node::CallStmt(l, _) | Node::Do(l, _) | Node::While(l, _, _) |
            Node::Repeat(l, _, _) | Node::If(l, _, _) | Node::ForNum(l, _, _, _, _, _) |
            Node::ForIn(l, _, _, _) | Node::Return(l, _) | Node::Break(l) |
            Node::Goto(l, _) | Node::Label(l, _) | Node::Chunk(l, _) => *l,
        }
    }
}
