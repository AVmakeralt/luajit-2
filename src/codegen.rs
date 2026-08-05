//! AST → VORTEX bytecode emitter.
//!
//! Uses the same scope-table model as the C version: function bodies
//! use a Lua table as local 0 (the "scope"), with all variable accesses
//! going through SCOPE_GET/SCOPE_SET runtime calls.

use crate::ast::{self, *};
use crate::runtime::{FnId, Runtime};
use crate::value::*;

/// VORTEX opcode constants (must match runtime/bytecode.h).
mod op {
    pub const HALT: u8 = 0;
    pub const NOP: u8 = 1;
    pub const LOAD_LOCAL: u8 = 2;
    pub const STORE_LOCAL: u8 = 3;
    pub const LOAD_FIELD: u8 = 4;
    pub const STORE_FIELD: u8 = 5;
    pub const LOAD_CONST_INT: u8 = 6;
    pub const LOAD_CONST_FLOAT: u8 = 7;
    pub const LOAD_CONST_STR: u8 = 8;
    pub const LOAD_NULL: u8 = 9;
    pub const LOAD_TRUE: u8 = 10;
    pub const LOAD_FALSE: u8 = 11;
    pub const LOAD_UNDEFINED: u8 = 12;
    pub const IADD: u8 = 13;
    pub const ISUB: u8 = 14;
    pub const IMUL: u8 = 15;
    pub const IDIV: u8 = 16;
    pub const IMOD: u8 = 17;
    pub const FADD: u8 = 18;
    pub const FSUB: u8 = 19;
    pub const FMUL: u8 = 20;
    pub const FDIV: u8 = 21;
    pub const ISHL: u8 = 22;
    pub const ISHR: u8 = 23;
    pub const IAND: u8 = 24;
    pub const IOR: u8 = 25;
    pub const IXOR: u8 = 26;
    pub const INEG: u8 = 27;
    pub const INOT: u8 = 28;
    pub const ICMP_EQ: u8 = 29;
    pub const ICMP_NE: u8 = 30;
    pub const ICMP_LT: u8 = 31;
    pub const ICMP_LE: u8 = 32;
    pub const ICMP_GT: u8 = 33;
    pub const ICMP_GE: u8 = 34;
    pub const FCMP_EQ: u8 = 35;
    pub const FCMP_NE: u8 = 36;
    pub const FCMP_LT: u8 = 37;
    pub const FCMP_LE: u8 = 38;
    pub const FCMP_GT: u8 = 39;
    pub const FCMP_GE: u8 = 40;
    pub const GOTO: u8 = 41;
    pub const IF_TRUE: u8 = 42;
    pub const IF_FALSE: u8 = 43;
    pub const CALL_STATIC: u8 = 44;
    pub const CALL_VIRTUAL: u8 = 45;
    pub const CALL_INTERFACE: u8 = 46;
    pub const RETURN: u8 = 47;
    pub const RETURN_VALUE: u8 = 48;
    // 49: RETURN_MULTI, 50-52: VARARG opcodes (not used)
    pub const NEW: u8 = 53;
    pub const NEWARRAY: u8 = 54;
    pub const CHECKCAST: u8 = 55;
    pub const INSTANCEOF: u8 = 56;
    pub const ARRAY_LOAD: u8 = 57;
    pub const ARRAY_STORE: u8 = 58;
    pub const ARRAY_LENGTH: u8 = 59;
    pub const THROW: u8 = 60;
    pub const CATCH: u8 = 61;
    pub const CATCH_TYPED: u8 = 62;
    pub const MONITOR_ENTER: u8 = 63;
    pub const MONITOR_EXIT: u8 = 64;
    pub const DUP: u8 = 65;
    pub const POP: u8 = 66;
    pub const SWAP: u8 = 67;
    pub const ISNULL: u8 = 68;
    pub const TYPEOF: u8 = 69;
    pub const CALL_RUNTIME: u8 = 70;
}

/// A compiled bytecode module.
pub struct Compiled {
    pub code: Vec<u8>,
    pub constants: Vec<Value>,
    pub max_locals: u16,
    pub max_stack: u16,
}

/// Per-function codegen state.
struct FuncCg {
    code: Vec<u8>,
    constants: Vec<Value>,
    max_locals: u16,
    max_stack: u16,
    cur_stack: u16,
    use_scope_table: bool,
    env_slot: u16,
    next_local: u16,
    next_label: u32,
    labels: Vec<(u32, usize)>,  // (label_id, code_offset)
    jumps: Vec<(usize, u32)>,   // (patch_offset, target_label)
    break_labels: Vec<u32>,
    loop_depth: u32,
    error: Option<String>,
    /// Local variable name→slot mappings (for the main chunk's VORTEX locals).
    locals: Vec<(String, u16)>,
}

impl FuncCg {
    fn new(use_scope_table: bool) -> Self {
        let mut f = FuncCg {
            code: Vec::new(),
            constants: Vec::new(),
            max_locals: 1,  // local 0 = env/scope
            max_stack: 0,
            cur_stack: 0,
            use_scope_table,
            env_slot: 0,
            next_local: 1,
            next_label: 0,
            labels: Vec::new(),
            jumps: Vec::new(),
            break_labels: Vec::new(),
            loop_depth: 0,
            error: None,
            locals: Vec::new(),
        };
        f
    }

    fn declare_local(&mut self, name: &str) -> u16 {
        let slot = self.next_local;
        self.next_local += 1;
        if self.next_local > self.max_locals { self.max_locals = self.next_local; }
        self.locals.push((name.to_string(), slot));
        slot
    }

    fn lookup_local(&self, name: &str) -> Option<u16> {
        for (n, slot) in self.locals.iter().rev() {
            if n == name { return Some(*slot); }
        }
        None
    }

    fn emit_byte(&mut self, b: u8) {
        self.code.push(b);
    }

    fn emit_op(&mut self, opcode: u8) {
        self.emit_byte(opcode);
    }

    fn emit_u16(&mut self, val: u16) {
        self.emit_byte((val >> 8) as u8);
        self.emit_byte((val & 0xFF) as u8);
    }

    fn patch_u16(&mut self, offset: usize, val: u16) {
        self.code[offset] = (val >> 8) as u8;
        self.code[offset + 1] = (val & 0xFF) as u8;
    }

    fn push1(&mut self) {
        self.cur_stack += 1;
        if self.cur_stack > self.max_stack {
            self.max_stack = self.cur_stack;
        }
    }

    fn pop1(&mut self) {
        if self.cur_stack > 0 { self.cur_stack -= 1; }
    }

    fn const_add_int(&mut self, v: i64) -> u16 {
        let val = if v < -(1 << 45) || v > (1 << 45) - 1 {
            make_double(v as f64)
        } else {
            make_smi(v)
        };
        self.constants.push(val);
        (self.constants.len() - 1) as u16
    }

    fn const_add_float(&mut self, v: f64) -> u16 {
        self.constants.push(make_double(v));
        (self.constants.len() - 1) as u16
    }

    fn const_add_str(&mut self, rt: &Runtime, s: &[u8]) -> u16 {
        let val = rt.intern_string(s);
        self.constants.push(val);
        (self.constants.len() - 1) as u16
    }

    fn label_alloc(&mut self) -> u32 {
        let id = self.next_label;
        self.next_label += 1;
        self.labels.push((id, 0));
        id
    }

    fn label_place(&mut self, id: u32) {
        for (lid, off) in &mut self.labels {
            if *lid == id {
                *off = self.code.len();
                return;
            }
        }
    }

    fn label_target(&self, id: u32) -> usize {
        for (lid, off) in &self.labels {
            if *lid == id { return *off; }
        }
        0
    }

    fn emit_goto(&mut self, target: u32) {
        self.emit_op(op::GOTO);
        let off = self.code.len();
        self.emit_u16(0); // placeholder
        self.jumps.push((off, target));
    }

    fn emit_if_true(&mut self, target: u32) {
        self.emit_op(op::IF_TRUE);
        let off = self.code.len();
        self.emit_u16(0);
        self.jumps.push((off, target));
        self.pop1();
    }

    fn emit_if_false(&mut self, target: u32) {
        self.emit_op(op::IF_FALSE);
        let off = self.code.len();
        self.emit_u16(0);
        self.jumps.push((off, target));
        self.pop1();
    }

    fn resolve_jumps(&mut self) {
        let jumps: Vec<(usize, u32)> = self.jumps.clone();
        for (off, target) in &jumps {
            let pc = self.label_target(*target);
            self.patch_u16(*off, pc as u16);
        }
    }

    fn emit_lua_call(&mut self, fn_id: FnId, argc: u16) {
        let id = fn_id as u16;
        self.emit_op(op::CALL_RUNTIME);
        // Pack fn_id and argc into the 16-bit operand:
        //   operand = (fn_id << 6) | (argc & 0x3F)
        // The callback unpacks them: fn_id = operand >> 6, argc = operand & 0x3F
        let operand = (id << 6) | (argc & 0x3F);
        self.emit_u16(operand);
        // stack: pop argc, push 1
        for _ in 0..argc { self.pop1(); }
        self.push1();
    }
}

/// Compile a chunk AST into a VORTEX bytecode module.
pub fn compile(rt: &Runtime, chunk: &Node) -> Result<Compiled, String> {
    let mut f = FuncCg::new(false); // main chunk: VORTEX locals
    compile_chunk(rt, &mut f, chunk);
    if let Some(e) = f.error.take() {
        return Err(e);
    }
    f.emit_op(op::RETURN);
    f.resolve_jumps();
    Ok(Compiled {
        code: f.code,
        constants: f.constants,
        max_locals: f.next_local.max(8),
        max_stack: f.max_stack.max(16),
    })
}

/// Compile a function body into a separate bytecode module.
pub fn compile_function_body(rt: &Runtime, proto: &FuncProto) -> Result<Compiled, String> {
    let mut f = FuncCg::new(true); // function body: scope-table model
    for stmt in &proto.body {
        compile_stmt(rt, &mut f, stmt);
        if f.error.is_some() { break; }
    }
    if let Some(e) = f.error.take() {
        return Err(e);
    }
    f.emit_op(op::RETURN);
    f.resolve_jumps();
    Ok(Compiled {
        code: f.code,
        constants: f.constants,
        max_locals: f.next_local,
        max_stack: f.max_stack,
    })
}

fn compile_chunk(rt: &Runtime, f: &mut FuncCg, node: &Node) {
    if let Node::Chunk(_, stmts) = node {
        for stmt in stmts {
            compile_stmt(rt, f, stmt);
            if f.error.is_some() { return; }
        }
    }
}

fn compile_stmt(rt: &Runtime, f: &mut FuncCg, node: &Node) {
    match node {
        Node::Local(_, names, vals) => compile_local(rt, f, names, vals),
        Node::Assign(_, targets, vals) => compile_assign(rt, f, targets, vals),
        Node::CallStmt(_, call) => {
            compile_expr(rt, f, call);
            f.emit_op(op::POP);
            f.pop1();
        }
        Node::Do(_, stmts) => {
            for s in stmts { compile_stmt(rt, f, s); }
        }
        Node::While(_, cond, body) => compile_while(rt, f, cond, body),
        Node::Repeat(_, body, cond) => compile_repeat(rt, f, body, cond),
        Node::If(_, branches, else_body) => compile_if(rt, f, branches, else_body),
        Node::ForNum(_, name, init, limit, step, body) => compile_for_num(rt, f, name, init, limit, step, body),
        Node::ForIn(_, names, exprs, body) => compile_for_in(rt, f, names, exprs, body),
        Node::Return(_, vals) => compile_return(rt, f, vals),
        Node::Break(_) => compile_break(f),
        Node::Goto(_, _) | Node::Label(_, _) => { /* no-op for MVP */ }
        _ => {
            // expression statement
            compile_expr(rt, f, node);
            f.emit_op(op::POP);
            f.pop1();
        }
    }
}

fn compile_local(rt: &Runtime, f: &mut FuncCg, names: &[String], vals: &[Node]) {
    for (i, name) in names.iter().enumerate() {
        if i < vals.len() {
            compile_expr(rt, f, &vals[i]);
        } else {
            f.emit_op(op::LOAD_NULL);
            f.push1();
        }
        if f.use_scope_table {
            // scope_declare(env, name, value)
            let tmp = f.next_local; f.next_local += 1;
            if f.next_local > f.max_locals { f.max_locals = f.next_local; }
            f.emit_op(op::STORE_LOCAL);
            f.emit_u16(tmp);
            f.pop1();
            f.emit_op(op::LOAD_LOCAL); f.emit_u16(f.env_slot); f.push1();
            let ni = f.const_add_str(rt, name.as_bytes());
            f.emit_op(op::LOAD_CONST_STR); f.emit_u16(ni); f.push1();
            f.emit_op(op::LOAD_LOCAL); f.emit_u16(tmp); f.push1();
            f.emit_lua_call(FnId::ScopeDeclare, 3);
            f.emit_op(op::POP); f.pop1();
        } else {
            let slot = f.declare_local(name);
            f.emit_op(op::STORE_LOCAL);
            f.emit_u16(slot);
            f.pop1();
            // Also store in global env for closures
            f.emit_op(op::LOAD_LOCAL); f.emit_u16(f.env_slot); f.push1();
            let ni = f.const_add_str(rt, name.as_bytes());
            f.emit_op(op::LOAD_CONST_STR); f.emit_u16(ni); f.push1();
            f.emit_op(op::LOAD_LOCAL); f.emit_u16(slot); f.push1();
            f.emit_lua_call(FnId::GlobalSet, 3);
            f.emit_op(op::POP); f.pop1();
        }
    }
}

fn compile_assign(rt: &Runtime, f: &mut FuncCg, targets: &[Node], vals: &[Node]) {
    for (i, target) in targets.iter().enumerate() {
        let val = vals.get(i);
        match target {
            Node::Name(_, name) => {
                if let Some(v) = val {
                    compile_expr(rt, f, v);
                } else {
                    f.emit_op(op::LOAD_NULL); f.push1();
                }
                if f.use_scope_table {
                    let tmp = f.next_local; f.next_local += 1;
                    if f.next_local > f.max_locals { f.max_locals = f.next_local; }
                    f.emit_op(op::STORE_LOCAL); f.emit_u16(tmp); f.pop1();
                    f.emit_op(op::LOAD_LOCAL); f.emit_u16(f.env_slot); f.push1();
                    let ni = f.const_add_str(rt, name.as_bytes());
                    f.emit_op(op::LOAD_CONST_STR); f.emit_u16(ni); f.push1();
                    f.emit_op(op::LOAD_LOCAL); f.emit_u16(tmp); f.push1();
                    f.emit_lua_call(FnId::ScopeSet, 3);
                    f.emit_op(op::POP); f.pop1();
                } else {
                    // Main chunk: store in global env (all main-chunk vars are "global" to closures)
                    let tmp = f.next_local; f.next_local += 1;
                    if f.next_local > f.max_locals { f.max_locals = f.next_local; }
                    f.emit_op(op::STORE_LOCAL); f.emit_u16(tmp); f.pop1();
                    f.emit_op(op::LOAD_LOCAL); f.emit_u16(f.env_slot); f.push1();
                    let ni = f.const_add_str(rt, name.as_bytes());
                    f.emit_op(op::LOAD_CONST_STR); f.emit_u16(ni); f.push1();
                    f.emit_op(op::LOAD_LOCAL); f.emit_u16(tmp); f.push1();
                    f.emit_lua_call(FnId::GlobalSet, 3);
                    f.emit_op(op::POP); f.pop1();
                }
            }
            Node::Field(_, obj, field_name) => {
                // t.name = value
                if let Some(v) = val { compile_expr(rt, f, v); }
                else { f.emit_op(op::LOAD_NULL); f.push1(); }
                let tmp = f.next_local; f.next_local += 1;
                if f.next_local > f.max_locals { f.max_locals = f.next_local; }
                f.emit_op(op::STORE_LOCAL); f.emit_u16(tmp); f.pop1();
                compile_expr(rt, f, obj);
                let ni = f.const_add_str(rt, field_name.as_bytes());
                f.emit_op(op::LOAD_CONST_STR); f.emit_u16(ni); f.push1();
                f.emit_op(op::LOAD_LOCAL); f.emit_u16(tmp); f.push1();
                f.emit_lua_call(FnId::SetField, 3);
                f.emit_op(op::POP); f.pop1();
            }
            Node::Index(_, obj, key) => {
                if let Some(v) = val { compile_expr(rt, f, v); }
                else { f.emit_op(op::LOAD_NULL); f.push1(); }
                let tmp = f.next_local; f.next_local += 1;
                if f.next_local > f.max_locals { f.max_locals = f.next_local; }
                f.emit_op(op::STORE_LOCAL); f.emit_u16(tmp); f.pop1();
                compile_expr(rt, f, obj);
                compile_expr(rt, f, key);
                f.emit_op(op::LOAD_LOCAL); f.emit_u16(tmp); f.push1();
                f.emit_lua_call(FnId::SetField, 3);
                f.emit_op(op::POP); f.pop1();
            }
            _ => {
                f.error = Some("cannot assign to this expression".to_string());
                return;
            }
        }
    }
}

fn compile_while(rt: &Runtime, f: &mut FuncCg, cond: &Node, body: &[Node]) {
    let start = f.label_alloc();
    let end = f.label_alloc();
    f.label_place(start);
    compile_expr(rt, f, cond);
    f.emit_if_false(end);
    f.loop_depth += 1;
    f.break_labels.push(end);
    for s in body { compile_stmt(rt, f, s); }
    f.break_labels.pop();
    f.loop_depth -= 1;
    f.emit_goto(start);
    f.label_place(end);
}

fn compile_repeat(rt: &Runtime, f: &mut FuncCg, body: &[Node], cond: &Node) {
    let start = f.label_alloc();
    let end = f.label_alloc();
    f.label_place(start);
    f.loop_depth += 1;
    f.break_labels.push(end);
    for s in body { compile_stmt(rt, f, s); }
    f.break_labels.pop();
    f.loop_depth -= 1;
    compile_expr(rt, f, cond);
    f.emit_if_false(start); // if cond is false, loop back
    f.label_place(end);
}

fn compile_if(rt: &Runtime, f: &mut FuncCg, branches: &[(Node, Vec<Node>)], else_body: &Option<Vec<Node>>) {
    let end = f.label_alloc();
    for (cond, body) in branches {
        compile_expr(rt, f, cond);
        let next = f.label_alloc();
        f.emit_if_false(next);
        for s in body { compile_stmt(rt, f, s); }
        f.emit_goto(end);
        f.label_place(next);
    }
    if let Some(eb) = else_body {
        for s in eb { compile_stmt(rt, f, s); }
    }
    f.label_place(end);
}

fn compile_for_num(rt: &Runtime, f: &mut FuncCg, name: &str, init: &Node, limit: &Node, step: &Option<Box<Node>>, body: &[Node]) {
    let v_slot = f.next_local; f.next_local += 1;
    let limit_slot = f.next_local; f.next_local += 1;
    let step_slot = f.next_local; f.next_local += 1;
    if f.next_local > f.max_locals { f.max_locals = f.next_local; }

    compile_expr(rt, f, init);
    f.emit_op(op::STORE_LOCAL); f.emit_u16(v_slot); f.pop1();
    compile_expr(rt, f, limit);
    f.emit_op(op::STORE_LOCAL); f.emit_u16(limit_slot); f.pop1();
    if let Some(s) = step {
        compile_expr(rt, f, s);
    } else {
        let ci = f.const_add_int(1);
        f.emit_op(op::LOAD_CONST_INT); f.emit_u16(ci); f.push1();
    }
    f.emit_op(op::STORE_LOCAL); f.emit_u16(step_slot); f.pop1();

    let start = f.label_alloc();
    let end = f.label_alloc();
    f.label_place(start);

    // Condition: limit < v (i.e. v > limit)
    f.emit_op(op::LOAD_LOCAL); f.emit_u16(limit_slot); f.push1();
    f.emit_op(op::LOAD_LOCAL); f.emit_u16(v_slot); f.push1();
    f.emit_lua_call(FnId::CmpLt, 2);
    f.emit_if_true(end);

    f.loop_depth += 1;
    f.break_labels.push(end);
    // Store v into scope table for body access
    if f.use_scope_table {
        f.emit_op(op::LOAD_LOCAL); f.emit_u16(f.env_slot); f.push1();
        let ni = f.const_add_str(rt, name.as_bytes());
        f.emit_op(op::LOAD_CONST_STR); f.emit_u16(ni); f.push1();
        f.emit_op(op::LOAD_LOCAL); f.emit_u16(v_slot); f.push1();
        f.emit_lua_call(FnId::ScopeDeclare, 3);
        f.emit_op(op::POP); f.pop1();
    } else {
        let user_slot = f.declare_local(name);
        f.emit_op(op::LOAD_LOCAL); f.emit_u16(v_slot); f.push1();
        f.emit_op(op::STORE_LOCAL); f.emit_u16(user_slot); f.pop1();
    }
    for s in body { compile_stmt(rt, f, s); }
    f.break_labels.pop();
    f.loop_depth -= 1;

    // v = v + step
    f.emit_op(op::LOAD_LOCAL); f.emit_u16(v_slot); f.push1();
    f.emit_op(op::LOAD_LOCAL); f.emit_u16(step_slot); f.push1();
    f.emit_lua_call(FnId::ArithAdd, 2);
    f.emit_op(op::STORE_LOCAL); f.emit_u16(v_slot); f.pop1();
    f.emit_goto(start);
    f.label_place(end);
}

fn compile_for_in(rt: &Runtime, f: &mut FuncCg, names: &[String], exprs: &[Node], body: &[Node]) {
    let f_slot = f.next_local; f.next_local += 1;
    let s_slot = f.next_local; f.next_local += 1;
    let var_slot = f.next_local; f.next_local += 1;
    if f.next_local > f.max_locals { f.max_locals = f.next_local; }

    // Special case: pairs(t) / ipairs(t)
    if exprs.len() == 1 {
        if let Node::Call(_, callee, args) = &exprs[0] {
            if let Node::Name(_, n) = callee.as_ref() {
                if (n == "pairs" || n == "ipairs") && args.len() >= 1 {
                    let is_pairs = n == "pairs";
                    // Evaluate table arg, store in s_slot
                    compile_expr(rt, f, &args[0]);
                    f.emit_op(op::STORE_LOCAL); f.emit_u16(s_slot); f.pop1();
                    // Call pairs/ipairs to get iterator
                    f.emit_op(op::LOAD_LOCAL); f.emit_u16(s_slot); f.push1();
                    f.emit_lua_call(if is_pairs { FnId::Pairs } else { FnId::IPairs }, 1);
                    f.emit_op(op::STORE_LOCAL); f.emit_u16(f_slot); f.pop1();
                    // var starts as nil
                    f.emit_op(op::LOAD_NULL); f.push1();
                    f.emit_op(op::STORE_LOCAL); f.emit_u16(var_slot); f.pop1();

                    let start = f.label_alloc();
                    let end = f.label_alloc();
                    f.label_place(start);
                    // f(s, var) → result
                    f.emit_op(op::LOAD_LOCAL); f.emit_u16(f_slot); f.push1();
                    f.emit_op(op::LOAD_LOCAL); f.emit_u16(s_slot); f.push1();
                    f.emit_op(op::LOAD_LOCAL); f.emit_u16(var_slot); f.push1();
                    f.emit_lua_call(FnId::Call, 3);
                    f.emit_op(op::DUP); f.push1();
                    f.emit_op(op::STORE_LOCAL); f.emit_u16(var_slot);
                    f.emit_if_false(end);
                    f.emit_op(op::POP); f.pop1();

                    // Assign loop variables
                    f.loop_depth += 1;
                    f.break_labels.push(end);
                    if names.len() >= 1 {
                        let val = if f.use_scope_table {
                            f.emit_op(op::LOAD_LOCAL); f.emit_u16(f.env_slot); f.push1();
                            let ni = f.const_add_str(rt, names[0].as_bytes());
                            f.emit_op(op::LOAD_CONST_STR); f.emit_u16(ni); f.push1();
                            f.emit_op(op::LOAD_LOCAL); f.emit_u16(var_slot); f.push1();
                            f.emit_lua_call(FnId::ScopeDeclare, 3);
                            f.emit_op(op::POP); f.pop1();
                        } else {
                            let user_slot = f.declare_local(&names[0]);
                            f.emit_op(op::LOAD_LOCAL); f.emit_u16(var_slot); f.push1();
                            f.emit_op(op::STORE_LOCAL); f.emit_u16(user_slot); f.pop1();
                        };
                    }
                    if names.len() >= 2 {
                        // v = s[var]
                        f.emit_op(op::LOAD_LOCAL); f.emit_u16(s_slot); f.push1();
                        f.emit_op(op::LOAD_LOCAL); f.emit_u16(var_slot); f.push1();
                        f.emit_lua_call(FnId::GetField, 2);
                        if f.use_scope_table {
                            f.emit_op(op::LOAD_LOCAL); f.emit_u16(f.env_slot); f.push1();
                            let ni = f.const_add_str(rt, names[1].as_bytes());
                            f.emit_op(op::LOAD_CONST_STR); f.emit_u16(ni); f.push1();
                            // value is on stack; reorder
                            let tmp = f.next_local; f.next_local += 1;
                            if f.next_local > f.max_locals { f.max_locals = f.next_local; }
                            f.emit_op(op::STORE_LOCAL); f.emit_u16(tmp); f.pop1();
                            f.emit_op(op::LOAD_LOCAL); f.emit_u16(f.env_slot); f.push1();
                            f.emit_op(op::LOAD_CONST_STR); f.emit_u16(ni); f.push1();
                            f.emit_op(op::LOAD_LOCAL); f.emit_u16(tmp); f.push1();
                            f.emit_lua_call(FnId::ScopeDeclare, 3);
                            f.emit_op(op::POP); f.pop1();
                        } else {
                            let user_slot = f.next_local; f.next_local += 1;
                            if f.next_local > f.max_locals { f.max_locals = f.next_local; }
                            f.emit_op(op::STORE_LOCAL); f.emit_u16(user_slot); f.pop1();
                        }
                    }
                    for s in body { compile_stmt(rt, f, s); }
                    f.break_labels.pop();
                    f.loop_depth -= 1;
                    f.emit_goto(start);
                    f.label_place(end);
                    f.emit_op(op::POP); f.pop1();
                    return;
                }
            }
        }
    }

    // Generic for-in (not pairs/ipairs)
    f.error = Some("for-in: only pairs(t)/ipairs(t) supported".to_string());
}

fn compile_return(rt: &Runtime, f: &mut FuncCg, vals: &[Node]) {
    if vals.is_empty() {
        f.emit_op(op::RETURN);
    } else {
        compile_expr(rt, f, &vals[0]);
        f.emit_op(op::RETURN_VALUE);
        f.pop1();
    }
}

fn compile_break(f: &mut FuncCg) {
    if f.break_labels.is_empty() {
        f.error = Some("'break' outside of a loop".to_string());
        return;
    }
    let target = *f.break_labels.last().unwrap();
    f.emit_goto(target);
}

fn compile_expr(rt: &Runtime, f: &mut FuncCg, node: &Node) {
    match node {
        Node::Nil(_) => { f.emit_op(op::LOAD_NULL); f.push1(); }
        Node::True(_) => { f.emit_op(op::LOAD_TRUE); f.push1(); }
        Node::False(_) => { f.emit_op(op::LOAD_FALSE); f.push1(); }
        Node::Int(_, v) => {
            let ci = f.const_add_int(*v);
            f.emit_op(op::LOAD_CONST_INT); f.emit_u16(ci); f.push1();
        }
        Node::Float(_, v) => {
            let ci = f.const_add_float(*v);
            f.emit_op(op::LOAD_CONST_FLOAT); f.emit_u16(ci); f.push1();
        }
        Node::Str(_, s) => {
            let ci = f.const_add_str(rt, s);
            f.emit_op(op::LOAD_CONST_STR); f.emit_u16(ci); f.push1();
        }
        Node::Dots(_) => {
            // varargs: return nil for MVP
            f.emit_op(op::LOAD_NULL); f.push1();
        }
        Node::Name(_, name) => compile_name(rt, f, name),
        Node::Index(_, obj, key) => {
            compile_expr(rt, f, obj);
            compile_expr(rt, f, key);
            f.emit_lua_call(FnId::GetField, 2);
        }
        Node::Field(_, obj, name) => {
            compile_expr(rt, f, obj);
            let ni = f.const_add_str(rt, name.as_bytes());
            f.emit_op(op::LOAD_CONST_STR); f.emit_u16(ni); f.push1();
            f.emit_lua_call(FnId::GetField, 2);
        }
        Node::Table(_, entries) => compile_table_ctor(rt, f, entries),
        Node::BinOp(_, op, l, r) => compile_binop(rt, f, op, l, r),
        Node::UnOp(_, op, operand) => compile_unop(rt, f, op, operand),
        Node::Call(_, callee, args) => compile_call(rt, f, callee, args),
        Node::MethodCall(_, recv, name, args) => compile_method_call(rt, f, recv, name, args),
        Node::Function(_, proto) => compile_function(rt, f, proto),
        _ => {
            f.error = Some(format!("unhandled expr: {:?}", node));
        }
    }
}

fn compile_name(rt: &Runtime, f: &mut FuncCg, name: &str) {
    if f.use_scope_table {
        // Function body: ScopeGet
        f.emit_op(op::LOAD_LOCAL); f.emit_u16(f.env_slot); f.push1();
        let ni = f.const_add_str(rt, name.as_bytes());
        f.emit_op(op::LOAD_CONST_STR); f.emit_u16(ni); f.push1();
        f.emit_lua_call(FnId::ScopeGet, 2);
    } else {
        // Main chunk: check VORTEX locals first, then global env
        if let Some(slot) = f.lookup_local(name) {
            f.emit_op(op::LOAD_LOCAL);
            f.emit_u16(slot);
            f.push1();
        } else {
            f.emit_op(op::LOAD_LOCAL); f.emit_u16(f.env_slot); f.push1();
            let ni = f.const_add_str(rt, name.as_bytes());
            f.emit_op(op::LOAD_CONST_STR); f.emit_u16(ni); f.push1();
            f.emit_lua_call(FnId::GlobalGet, 2);
        }
    }
}

fn compile_table_ctor(rt: &Runtime, f: &mut FuncCg, entries: &[ast::TableEntry]) {
    // Create empty table: new_table(0, 0)
    let ci0 = f.const_add_int(0);
    f.emit_op(op::LOAD_CONST_INT); f.emit_u16(ci0); f.push1();
    let ci0b = f.const_add_int(0);
    f.emit_op(op::LOAD_CONST_INT); f.emit_u16(ci0b); f.push1();
    f.emit_lua_call(FnId::NewTable, 2);

    let mut pos = 1i64;
    for entry in entries {
        match entry {
            ast::TableEntry::Positional(val) => {
                f.emit_op(op::DUP); f.push1();
                let ki = f.const_add_int(pos); pos += 1;
                f.emit_op(op::LOAD_CONST_INT); f.emit_u16(ki); f.push1();
                compile_expr(rt, f, val);
                f.emit_lua_call(FnId::SetField, 3);
                f.emit_op(op::POP); f.pop1();
            }
            ast::TableEntry::Keyed(key, val) => {
                f.emit_op(op::DUP); f.push1();
                compile_expr(rt, f, key);
                compile_expr(rt, f, val);
                f.emit_lua_call(FnId::SetField, 3);
                f.emit_op(op::POP); f.pop1();
            }
            ast::TableEntry::Named(name, val) => {
                f.emit_op(op::DUP); f.push1();
                let ni = f.const_add_str(rt, name.as_bytes());
                f.emit_op(op::LOAD_CONST_STR); f.emit_u16(ni); f.push1();
                compile_expr(rt, f, val);
                f.emit_lua_call(FnId::SetField, 3);
                f.emit_op(op::POP); f.pop1();
            }
        }
    }
}

fn compile_binop(rt: &Runtime, f: &mut FuncCg, op: &BinOp, l: &Node, r: &Node) {
    match op {
        BinOp::And => {
            compile_expr(rt, f, l);
            f.emit_op(op::DUP); f.push1();
            let end = f.label_alloc();
            f.emit_if_false(end);
            f.emit_op(op::POP); f.pop1();
            compile_expr(rt, f, r);
            f.label_place(end);
        }
        BinOp::Or => {
            compile_expr(rt, f, l);
            f.emit_op(op::DUP); f.push1();
            let end = f.label_alloc();
            f.emit_if_true(end);
            f.emit_op(op::POP); f.pop1();
            compile_expr(rt, f, r);
            f.label_place(end);
        }
        _ => {
            compile_expr(rt, f, l);
            compile_expr(rt, f, r);
            let fn_id = match op {
                BinOp::Add => FnId::ArithAdd,
                BinOp::Sub => FnId::ArithSub,
                BinOp::Mul => FnId::ArithMul,
                BinOp::Div => FnId::ArithDiv,
                BinOp::IDiv => FnId::ArithIDiv,
                BinOp::Mod => FnId::ArithMod,
                BinOp::Pow => FnId::ArithPow,
                BinOp::Concat => FnId::ArithConcat,
                BinOp::Eq => FnId::CmpEq,
                BinOp::Ne => FnId::CmpEq, // negate below
                BinOp::Lt => FnId::CmpLt,
                BinOp::Le => FnId::CmpLe,
                BinOp::Gt => FnId::CmpLt, // swap
                BinOp::Ge => FnId::CmpLe, // swap
                BinOp::BAnd => FnId::BitAnd,
                BinOp::BOr => FnId::BitOr,
                BinOp::BXor => FnId::BitXor,
                BinOp::Shl => FnId::BitShl,
                BinOp::Shr => FnId::BitShr,
                BinOp::And | BinOp::Or => unreachable!(),
            };
            if matches!(op, BinOp::Gt | BinOp::Ge) {
                // Swap operands: emit SWAP before the call
                f.emit_op(op::SWAP);
            }
            f.emit_lua_call(fn_id, 2);
            if matches!(op, BinOp::Ne) {
                // Negate the boolean result
                let true_l = f.label_alloc();
                let end_l = f.label_alloc();
                f.emit_op(op::DUP); f.push1();
                f.emit_if_true(true_l);
                f.emit_op(op::POP); f.pop1();
                f.emit_op(op::LOAD_TRUE); f.push1();
                f.emit_goto(end_l);
                f.label_place(true_l);
                f.emit_op(op::POP); f.pop1();
                f.emit_op(op::LOAD_FALSE); f.push1();
                f.label_place(end_l);
            }
        }
    }
}

fn compile_unop(rt: &Runtime, f: &mut FuncCg, op: &UnOp, operand: &Node) {
    match op {
        UnOp::Neg => {
            // 0 - operand
            let zi = f.const_add_int(0);
            f.emit_op(op::LOAD_CONST_INT); f.emit_u16(zi); f.push1();
            compile_expr(rt, f, operand);
            f.emit_op(op::SWAP);
            f.emit_lua_call(FnId::ArithSub, 2);
        }
        UnOp::Not => {
            compile_expr(rt, f, operand);
            let true_l = f.label_alloc();
            let end_l = f.label_alloc();
            f.emit_op(op::DUP); f.push1();
            f.emit_if_true(true_l);
            f.emit_op(op::POP); f.pop1();
            f.emit_op(op::LOAD_TRUE); f.push1();
            f.emit_goto(end_l);
            f.label_place(true_l);
            f.emit_op(op::POP); f.pop1();
            f.emit_op(op::LOAD_FALSE); f.push1();
            f.label_place(end_l);
        }
        UnOp::Len => {
            compile_expr(rt, f, operand);
            f.emit_lua_call(FnId::Length, 1);
        }
        UnOp::BNot => {
            // operand XOR -1
            compile_expr(rt, f, operand);
            let mi = f.const_add_int(-1);
            f.emit_op(op::LOAD_CONST_INT); f.emit_u16(mi); f.push1();
            f.emit_op(op::SWAP);
            f.emit_lua_call(FnId::BitXor, 2);
        }
    }
}

fn compile_call(rt: &Runtime, f: &mut FuncCg, callee: &Node, args: &[Node]) {
    compile_expr(rt, f, callee);
    for a in args {
        compile_expr(rt, f, a);
    }
    f.emit_lua_call(FnId::Call, (1 + args.len()) as u16);
}

fn compile_method_call(rt: &Runtime, f: &mut FuncCg, recv: &Node, name: &str, args: &[Node]) {
    compile_expr(rt, f, recv);
    let ni = f.const_add_str(rt, name.as_bytes());
    f.emit_op(op::LOAD_CONST_STR); f.emit_u16(ni); f.push1();
    for a in args {
        compile_expr(rt, f, a);
    }
    f.emit_lua_call(FnId::CallMethod, (2 + args.len()) as u16);
}

fn compile_function(rt: &Runtime, f: &mut FuncCg, proto: &FuncProto) {
    // Register the proto and compile its body
    let proto_id = rt.register_proto(proto.clone());
    match crate::codegen::compile_function_body(rt, proto) {
        Ok(compiled) => {
            rt.set_proto_bytecode(proto_id, compiled);
        }
        Err(e) => {
            f.error = Some(format!("failed to compile function: {}", e));
            return;
        }
    }
    // Emit: new_closure_with_env(proto_id, env)
    let pi = f.const_add_int(proto_id as i64);
    f.emit_op(op::LOAD_CONST_INT); f.emit_u16(pi); f.push1();
    f.emit_op(op::LOAD_LOCAL); f.emit_u16(f.env_slot); f.push1();
    f.emit_lua_call(FnId::NewClosureWithEnv, 2);
}
