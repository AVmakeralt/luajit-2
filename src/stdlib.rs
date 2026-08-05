//! Lua standard library — implemented in Rust.

use crate::runtime::{FnId, Runtime};
use crate::value::*;
use std::os::raw::c_void;
use std::rc::Rc;
use std::cell::RefCell;

type Args<'a> = &'a [Value];

/// Dispatch a CALL_RUNTIME with the given fn_id.
pub fn dispatch(rt: &Runtime, fn_id: u16, args: Args) -> Value {
    let id = FnId::from_u16(fn_id);
    match id {
        Some(FnId::Print) => do_print(args),
        Some(FnId::ToString) => rt.intern_string(&to_lua_string(args[0])),
        Some(FnId::Type) => rt.intern_string(lua_type_name(args[0]).as_bytes()),
        Some(FnId::Assert) => do_assert(rt, args),
        Some(FnId::Error) => do_error(rt, args),
        Some(FnId::ToNumber) => do_tonumber(args),
        Some(FnId::RawGet) => as_table(args[0]).map_or(NULL, |t| t.get(args[1])),
        Some(FnId::RawSet) => { if let Some(t)=as_table(args[0]) { t.set(args[1], args[2]); } args[0] }
        Some(FnId::RawEqual) => make_bool(raw_equal(args[0], args[1])),
        Some(FnId::RawLen) => do_rawlen(args),
        Some(FnId::Pairs) => make_function(LuaFunction::new_native(native_pairs_iter)),
        Some(FnId::IPairs) => make_function(LuaFunction::new_native(native_ipairs_iter)),
        Some(FnId::Next) => as_table(args[0]).map_or(NULL, |t| t.next_key(args.get(1).copied().unwrap_or(UNDEFINED))),
        Some(FnId::Unpack) => do_unpack(rt, args),
        Some(FnId::SetMetatable) => do_setmetatable(args),
        Some(FnId::GetMetatable) => do_getmetatable(args),
        Some(FnId::Select) => do_select(args),
        Some(FnId::Pcall) => make_bool(true), // simplified
        // Arithmetic
        Some(FnId::ArithAdd) => arith(args, |a,b| a+b, |a,b| a.wrapping_add(b)),
        Some(FnId::ArithSub) => arith(args, |a,b| a-b, |a,b| a.wrapping_sub(b)),
        Some(FnId::ArithMul) => arith(args, |a,b| a*b, |a,b| a.wrapping_mul(b)),
        Some(FnId::ArithDiv) => arith_div(args),
        Some(FnId::ArithIDiv) => arith_idiv(args),
        Some(FnId::ArithMod) => arith_mod(args),
        Some(FnId::ArithPow) => make_double(to_double(args[0]).powf(to_double(args[1]))),
        Some(FnId::ArithConcat) => rt.intern_string(&{
            let mut v = to_lua_string(args[0]); v.extend_from_slice(&to_lua_string(args[1])); v
        }),
        // Comparisons
        Some(FnId::CmpEq) => make_bool(raw_equal(args[0], args[1])),
        Some(FnId::CmpLt) => cmp_lt(args),
        Some(FnId::CmpLe) => cmp_le(args),
        // Bitwise
        Some(FnId::BitAnd) => bit_op(args, |a,b| a&b),
        Some(FnId::BitOr) => bit_op(args, |a,b| a|b),
        Some(FnId::BitXor) => bit_op(args, |a,b| a^b),
        Some(FnId::BitShl) => bit_shift(args, |a,b| a.wrapping_shl(b as u32)),
        Some(FnId::BitShr) => bit_shift(args, |a,b| (a as u64).wrapping_shr(b as u32) as i64),
        // String
        Some(FnId::StrLen) => make_smi(as_string(args[0]).map_or(0, |s| s.len as i64)),
        Some(FnId::StrSub) => do_str_sub(rt, args),
        Some(FnId::StrUpper) => do_str_upper(rt, args),
        Some(FnId::StrLower) => do_str_lower(rt, args),
        Some(FnId::StrRep) => do_str_rep(rt, args),
        Some(FnId::StrReverse) => do_str_reverse(rt, args),
        Some(FnId::StrByte) => do_str_byte(args),
        Some(FnId::StrChar) => do_str_char(rt, args),
        Some(FnId::StrFormat) => do_str_format(rt, args),
        Some(FnId::StrFind) => do_str_find(args),
        Some(FnId::StrMatch) => do_str_match(rt, args),
        Some(FnId::StrGsub) => do_str_gsub(rt, args),
        // Table
        Some(FnId::TblInsert) => do_tbl_insert(args),
        Some(FnId::TblRemove) => do_tbl_remove(args),
        Some(FnId::TblConcat) => do_tbl_concat(rt, args),
        Some(FnId::TblSort) => do_tbl_sort(args),
        Some(FnId::TblMove) => do_tbl_move(args),
        Some(FnId::TblPack) => do_tbl_pack(rt, args),
        // Math
        Some(FnId::MathAbs) => math_unary(args, |a| a.abs(), |a| a.abs()),
        Some(FnId::MathFloor) => math_unary(args, |a| a.floor(), |a| a),
        Some(FnId::MathCeil) => math_unary(args, |a| a.ceil(), |a| a),
        Some(FnId::MathSqrt) => make_double(to_double(args[0]).sqrt()),
        Some(FnId::MathSin) => make_double(to_double(args[0]).sin()),
        Some(FnId::MathCos) => make_double(to_double(args[0]).cos()),
        Some(FnId::MathTan) => make_double(to_double(args[0]).tan()),
        Some(FnId::MathLog) => {
            if args.len() >= 2 { make_double(to_double(args[0]).log(to_double(args[1]))) }
            else { make_double(to_double(args[0]).ln()) }
        }
        Some(FnId::MathExp) => make_double(to_double(args[0]).exp()),
        Some(FnId::MathPow) => make_double(to_double(args[0]).powf(to_double(args[1]))),
        Some(FnId::MathMax) => math_minmax(args, |a,b| a > b),
        Some(FnId::MathMin) => math_minmax(args, |a,b| a < b),
        Some(FnId::MathRandom) => do_math_random(args),
        // I/O
        Some(FnId::IoWrite) => { use std::io::Write; let mut o = std::io::stdout().lock(); for a in args { let _ = o.write_all(&to_lua_string(*a)); } NULL }
        Some(FnId::IoRead) => {
            use std::io::BufRead;
            let mut line = String::new();
            let _ = std::io::stdin().lock().read_line(&mut line);
            while line.ends_with('\n') || line.ends_with('\r') { line.pop(); }
            rt.intern_string(line.as_bytes())
        }
        // OS
        Some(FnId::OsTime) => make_smi(std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).unwrap_or_default().as_secs() as i64),
        Some(FnId::OsClock) => make_double(std::time::Instant::now().elapsed().as_secs_f64()),
        Some(FnId::OsDate) => {
            let now = std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).unwrap_or_default().as_secs();
            rt.intern_string(format!("epoch: {}", now).as_bytes())
        }
        Some(FnId::OsGetenv) => {
            if let Some(s) = as_string(args[0]) {
                if let Ok(val) = std::env::var(s.as_str()) { return rt.intern_string(val.as_bytes()); }
            }
            NULL
        }
        Some(FnId::OsExecute) => {
            if let Some(s) = as_string(args[0]) {
                use std::process::Command;
                return make_smi(Command::new("sh").arg("-c").arg(s.as_str()).status().map(|s| s.code().unwrap_or(-1) as i64).unwrap_or(-1));
            }
            make_smi(0)
        }
        Some(FnId::OsExit) => {
            let code = if !args.is_empty() && is_smi(args[0]) { smi_value(args[0]) as i32 } else { 0 };
            std::process::exit(code);
        }
        // Function call / scope
        Some(FnId::Call) => {
            if args.is_empty() { return NULL; }
            rt.call(args[0], &args[1..])
        }
        Some(FnId::CallMethod) => do_call_method(rt, args),
        Some(FnId::NewTable) => make_table(LuaTable::new()),
        Some(FnId::SetField) => { if let Some(t)=as_table(args[0]) { t.set(args[1], args[2]); return args[0]; } NULL }
        Some(FnId::GetField) => as_table(args[0]).map_or(NULL, |t| t.get(args[1])),
        Some(FnId::NewClosure) => {
            if is_smi(args[0]) {
                let proto_id = smi_value(args[0]) as i32;
                let mut f = LuaFunction::new_lua(proto_id);
                f.captured_env = Some(make_table(rt.globals.clone()));
                return make_function(f);
            }
            NULL
        }
        Some(FnId::NewClosureWithEnv) => {
            if is_smi(args[0]) {
                let proto_id = smi_value(args[0]) as i32;
                let env = args[1];
                let mut f = LuaFunction::new_lua(proto_id);
                f.captured_env = Some(env);
                f.compiled_bc = rt.get_proto_bytecode(proto_id).map(|p| p as *mut c_void);
                return make_function(f);
            }
            NULL
        }
        Some(FnId::Length) => {
            if is_string(args[0]) { return make_smi(as_string(args[0]).unwrap().len as i64); }
            if is_table(args[0]) { return make_smi(as_table(args[0]).unwrap().array_len()); }
            NULL
        }
        Some(FnId::GlobalGet) => rt.globals.get(args[1]),
        Some(FnId::GlobalSet) => { rt.globals.set(args[1], args[2]); NULL }
        Some(FnId::ScopeGet) => do_scope_get(rt, args),
        Some(FnId::ScopeSet) => do_scope_set(rt, args),
        Some(FnId::ScopeDeclare) => { if let Some(t)=as_table(args[0]) { t.set(args[1], args[2]); } NULL }
        Some(FnId::NewScope) => {
            let scope = LuaTable::new();
            scope.set(rt.intern_string(b"__parent"), args.get(0).copied().unwrap_or(make_table(rt.globals.clone())));
            make_table(scope)
        }
        Some(FnId::Vararg) => NULL,
        Some(FnId::Require) => do_require(rt, args),
        Some(FnId::Dofile) => do_dofile(rt, args),
        None => { *rt.error_msg.borrow_mut() = Some(format!("unknown fn_id {}", fn_id)); NULL }
    }
}

impl FnId {
    pub fn from_u16(v: u16) -> Option<FnId> {
        Some(match v {
            100 => FnId::Print, 101 => FnId::ToString, 102 => FnId::ToNumber,
            103 => FnId::Type, 104 => FnId::Assert, 105 => FnId::Error,
            106 => FnId::Pcall, 107 => FnId::Select, 108 => FnId::RawGet,
            109 => FnId::RawSet, 110 => FnId::RawEqual, 111 => FnId::RawLen,
            112 => FnId::Next, 113 => FnId::Pairs, 114 => FnId::IPairs,
            115 => FnId::Unpack, 116 => FnId::SetMetatable, 117 => FnId::GetMetatable,
            120 => FnId::ArithAdd, 121 => FnId::ArithSub, 122 => FnId::ArithMul,
            123 => FnId::ArithDiv, 124 => FnId::ArithIDiv, 125 => FnId::ArithMod,
            126 => FnId::ArithPow, 127 => FnId::ArithConcat,
            130 => FnId::CmpEq, 131 => FnId::CmpLt, 132 => FnId::CmpLe,
            140 => FnId::BitAnd, 141 => FnId::BitOr, 142 => FnId::BitXor,
            143 => FnId::BitShl, 144 => FnId::BitShr,
            150 => FnId::StrLen, 151 => FnId::StrSub, 152 => FnId::StrUpper,
            153 => FnId::StrLower, 154 => FnId::StrRep, 155 => FnId::StrReverse,
            156 => FnId::StrByte, 157 => FnId::StrChar, 158 => FnId::StrFormat,
            159 => FnId::StrFind, 251 => FnId::StrGsub, 252 => FnId::StrMatch,
            170 => FnId::TblInsert, 171 => FnId::TblRemove, 172 => FnId::TblConcat,
            173 => FnId::TblSort, 270 => FnId::TblMove, 271 => FnId::TblPack,
            180 => FnId::MathAbs, 181 => FnId::MathFloor, 182 => FnId::MathCeil,
            183 => FnId::MathSqrt, 184 => FnId::MathSin, 185 => FnId::MathCos,
            186 => FnId::MathTan, 187 => FnId::MathLog, 188 => FnId::MathExp,
            189 => FnId::MathPow, 190 => FnId::MathMax, 191 => FnId::MathMin,
            192 => FnId::MathRandom,
            200 => FnId::IoWrite, 201 => FnId::IoRead,
            210 => FnId::OsTime, 211 => FnId::OsClock, 212 => FnId::OsDate,
            280 => FnId::OsGetenv, 281 => FnId::OsExecute, 282 => FnId::OsExit,
            220 => FnId::Call, 221 => FnId::CallMethod, 222 => FnId::NewTable,
            223 => FnId::SetField, 224 => FnId::GetField, 225 => FnId::NewClosure,
            226 => FnId::Vararg, 227 => FnId::Length, 228 => FnId::GlobalGet,
            229 => FnId::GlobalSet,
            240 => FnId::ScopeGet, 241 => FnId::ScopeSet, 242 => FnId::ScopeDeclare,
            243 => FnId::NewScope, 244 => FnId::NewClosureWithEnv,
            300 => FnId::Require, 301 => FnId::Dofile,
            _ => return None,
        })
    }
}

// ---- Helpers ----
fn to_double(v: Value) -> f64 {
    if is_smi(v) { smi_value(v) as f64 }
    else if is_double(v) { double_value(v) }
    else if is_string(v) {
        as_string(v).and_then(|s| std::str::from_utf8(&s.data).ok())
            .and_then(|s| s.parse::<f64>().ok()).unwrap_or(0.0)
    } else { 0.0 }
}

fn to_int(v: Value) -> Option<i64> {
    if is_smi(v) { Some(smi_value(v)) }
    else if is_double(v) {
        let d = double_value(v);
        if d.fract() == 0.0 && d >= i64::MIN as f64 && d <= i64::MAX as f64 { Some(d as i64) } else { None }
    } else { None }
}

const SMI_MIN: i64 = -(1i64 << 45);
const SMI_MAX: i64 = (1i64 << 45) - 1;

fn make_smi_if_fits(v: i64) -> Value {
    if v >= SMI_MIN && v <= SMI_MAX { make_smi(v) } else { make_double(v as f64) }
}

// ---- Basic functions ----
fn do_print(args: Args) -> Value {
    use std::io::Write;
    let mut o = std::io::stdout().lock();
    for (i, a) in args.iter().enumerate() {
        if i > 0 { let _ = write!(o, "\t"); }
        let _ = o.write_all(&to_lua_string(*a));
    }
    let _ = writeln!(o);
    NULL
}

fn do_assert(rt: &Runtime, args: Args) -> Value {
    if !is_truthy(args[0]) {
        let msg = if args.len() >= 2 && is_string(args[1]) {
            as_string(args[1]).unwrap().as_str().to_string()
        } else { "assertion failed!".to_string() };
        *rt.error_msg.borrow_mut() = Some(msg);
    }
    args[0]
}

fn do_error(rt: &Runtime, args: Args) -> Value {
    let msg = if is_string(args[0]) {
        as_string(args[0]).unwrap().as_str().to_string()
    } else {
        String::from_utf8_lossy(&to_lua_string(args[0])).to_string()
    };
    *rt.error_msg.borrow_mut() = Some(msg);
    NULL
}

fn do_tonumber(args: Args) -> Value {
    if args.is_empty() { return NULL; }
    let v = args[0];
    if is_smi(v) || is_double(v) { return v; }
    if is_string(v) {
        let s = as_string(v).unwrap();
        let s = std::str::from_utf8(&s.data).unwrap_or("");
        if let Ok(i) = s.parse::<i64>() { return make_smi_if_fits(i); }
        if let Ok(f) = s.parse::<f64>() { return make_double(f); }
    }
    NULL
}

fn do_rawlen(args: Args) -> Value {
    if is_string(args[0]) { return make_smi(as_string(args[0]).unwrap().len as i64); }
    if is_table(args[0]) { return make_smi(as_table(args[0]).unwrap().array_len()); }
    make_smi(0)
}

fn do_select(args: Args) -> Value {
    if args.is_empty() { return NULL; }
    if is_smi(args[0]) {
        let n = smi_value(args[0]);
        if n < 0 {
            let idx = (args.len() as i64) + n;
            if idx >= 1 && (idx as usize) < args.len() { return args[idx as usize]; }
        } else if n >= 1 && (n as usize) < args.len() {
            return args[n as usize];
        }
        return NULL;
    }
    make_smi((args.len() - 1) as i64)
}

fn do_unpack(rt: &Runtime, args: Args) -> Value {
    if let Some(t) = as_table(args[0]) {
        let i = if args.len() >= 2 { to_int(args[1]).unwrap_or(1) } else { 1 };
        let j = if args.len() >= 3 { to_int(args[2]).unwrap_or(t.array_len()) } else { t.array_len() };
        if i <= j { return t.get(make_smi(i)); }
    }
    NULL
}

fn do_setmetatable(args: Args) -> Value {
    if let Some(t) = as_table(args[0]) {
        if args.len() >= 2 && is_table(args[1]) {
            if let Some(mt) = as_table(args[1]) {
                *t.metatable.borrow_mut() = Some(Rc::new(LuaTable {
                    kind: mt.kind, _mark: mt._mark,
                    entries: RefCell::new(mt.entries.borrow().clone()),
                    metatable: RefCell::new(mt.metatable.borrow().clone()),
                }));
            }
        } else {
            *t.metatable.borrow_mut() = None;
        }
        return args[0];
    }
    NULL
}

fn do_getmetatable(args: Args) -> Value {
    if let Some(t) = as_table(args[0]) {
        if let Some(mt) = t.metatable.borrow().as_ref() {
            return make_table(mt.clone());
        }
    }
    NULL
}

// ---- Native pairs/ipairs iterators ----
pub extern "C" fn native_pairs_iter(args: Args, _ud: *mut c_void) -> Value {
    if let Some(t) = as_table(args[0]) {
        let prev = if args.len() >= 2 && !is_nil(args[1]) { args[1] } else { UNDEFINED };
        return t.next_key(prev);
    }
    NULL
}

pub extern "C" fn native_ipairs_iter(args: Args, _ud: *mut c_void) -> Value {
    if let Some(t) = as_table(args[0]) {
        let prev = if args.len() >= 2 && !is_nil(args[1]) { to_int(args[1]).unwrap_or(0) } else { 0 };
        let i = prev + 1;
        let v = t.get(make_smi(i));
        if is_nil(v) { return NULL; }
        return make_smi(i);
    }
    NULL
}

// ---- Arithmetic ----
fn arith(args: Args, f64_fn: impl Fn(f64, f64) -> f64, i64_fn: impl Fn(i64, i64) -> i64) -> Value {
    if is_smi(args[0]) && is_smi(args[1]) {
        let r = i64_fn(smi_value(args[0]), smi_value(args[1]));
        return make_smi_if_fits(r);
    }
    make_double(f64_fn(to_double(args[0]), to_double(args[1])))
}

fn arith_div(args: Args) -> Value {
    let a = to_double(args[0]); let b = to_double(args[1]);
    if b == 0.0 {
        if a == 0.0 { return make_double(f64::NAN); }
        return make_double(if a > 0.0 { f64::INFINITY } else { f64::NEG_INFINITY });
    }
    if is_smi(args[0]) && is_smi(args[1]) {
        let ia = smi_value(args[0]); let ib = smi_value(args[1]);
        if ia % ib == 0 { return make_smi(ia / ib); }
    }
    make_double(a / b)
}

fn arith_idiv(args: Args) -> Value {
    let a = to_double(args[0]); let b = to_double(args[1]);
    if b == 0.0 { return make_double(f64::NAN); }
    let r = (a / b).floor();
    make_smi_if_fits(r as i64)
}

fn arith_mod(args: Args) -> Value {
    let a = to_double(args[0]); let b = to_double(args[1]);
    if b == 0.0 { return make_double(f64::NAN); }
    let r = a - (a / b).floor() * b;
    if is_smi(args[0]) && is_smi(args[1]) && r.fract() == 0.0 { return make_smi(r as i64); }
    make_double(r)
}

// ---- Comparisons ----
fn cmp_lt(args: Args) -> Value {
    if is_smi(args[0]) && is_smi(args[1]) { return make_bool(smi_value(args[0]) < smi_value(args[1])); }
    if (is_smi(args[0]) || is_double(args[0])) && (is_smi(args[1]) || is_double(args[1])) {
        return make_bool(to_double(args[0]) < to_double(args[1]));
    }
    if is_string(args[0]) && is_string(args[1]) {
        return make_bool(as_string(args[0]).unwrap().data < as_string(args[1]).unwrap().data);
    }
    make_bool(false)
}

fn cmp_le(args: Args) -> Value {
    if is_smi(args[0]) && is_smi(args[1]) { return make_bool(smi_value(args[0]) <= smi_value(args[1])); }
    if (is_smi(args[0]) || is_double(args[0])) && (is_smi(args[1]) || is_double(args[1])) {
        return make_bool(to_double(args[0]) <= to_double(args[1]));
    }
    if is_string(args[0]) && is_string(args[1]) {
        return make_bool(as_string(args[0]).unwrap().data <= as_string(args[1]).unwrap().data);
    }
    make_bool(false)
}

// ---- Bitwise ----
fn bit_op(args: Args, op: impl Fn(i64, i64) -> i64) -> Value {
    make_smi(op(to_int(args[0]).unwrap_or(0), to_int(args[1]).unwrap_or(0)))
}

fn bit_shift(args: Args, op: impl Fn(i64, i64) -> i64) -> Value {
    make_smi(op(to_int(args[0]).unwrap_or(0), to_int(args[1]).unwrap_or(0)))
}

// ---- String functions ----
fn do_str_sub(rt: &Runtime, args: Args) -> Value {
    let s = match as_string(args[0]) { Some(s) => s, None => return rt.intern_string(b"") };
    let len = s.data.len() as i64;
    let i = if args.len() >= 2 { to_int(args[1]).unwrap_or(1) } else { 1 };
    let j = if args.len() >= 3 { to_int(args[2]).unwrap_or(len) } else { len };
    let i = if i < 0 { len + 1 + i } else { i };
    let j = if j < 0 { len + 1 + j } else { j };
    let i = i.max(1); let j = j.min(len);
    if i > j { return rt.intern_string(b""); }
    rt.intern_string(&s.data[((i-1) as usize)..(j as usize)])
}

fn do_str_upper(rt: &Runtime, args: Args) -> Value {
    if let Some(s) = as_string(args[0]) {
        return rt.intern_string(&s.data.iter().map(|&c| c.to_ascii_uppercase()).collect::<Vec<u8>>());
    }
    rt.intern_string(b"")
}

fn do_str_lower(rt: &Runtime, args: Args) -> Value {
    if let Some(s) = as_string(args[0]) {
        return rt.intern_string(&s.data.iter().map(|&c| c.to_ascii_lowercase()).collect::<Vec<u8>>());
    }
    rt.intern_string(b"")
}

fn do_str_rep(rt: &Runtime, args: Args) -> Value {
    if let Some(s) = as_string(args[0]) {
        let n = to_int(args[1]).unwrap_or(0);
        if n <= 0 { return rt.intern_string(b""); }
        let mut buf = Vec::with_capacity(s.data.len() * n as usize);
        for _ in 0..n { buf.extend_from_slice(&s.data); }
        return rt.intern_string(&buf);
    }
    rt.intern_string(b"")
}

fn do_str_reverse(rt: &Runtime, args: Args) -> Value {
    if let Some(s) = as_string(args[0]) {
        let mut r = s.data.clone(); r.reverse();
        return rt.intern_string(&r);
    }
    rt.intern_string(b"")
}

fn do_str_byte(args: Args) -> Value {
    if let Some(s) = as_string(args[0]) {
        let i = if args.len() >= 2 { to_int(args[1]).unwrap_or(1) } else { 1 };
        if i >= 1 && (i as usize) <= s.data.len() { return make_smi(s.data[(i-1) as usize] as i64); }
    }
    NULL
}

fn do_str_char(rt: &Runtime, args: Args) -> Value {
    let buf: Vec<u8> = args.iter().map(|v| to_int(*v).unwrap_or(0) as u8).collect();
    rt.intern_string(&buf)
}

fn do_str_format(rt: &Runtime, args: Args) -> Value {
    let fmt = match as_string(args[0]) { Some(s) => s, None => return rt.intern_string(b"") };
    let fmt_str = std::str::from_utf8(&fmt.data).unwrap_or("");
    let mut result = String::new();
    let mut arg_i = 1;
    let chars: Vec<char> = fmt_str.chars().collect();
    let mut i = 0;
    while i < chars.len() {
        if chars[i] != '%' { result.push(chars[i]); i += 1; continue; }
        i += 1;
        if i >= chars.len() { break; }
        match chars[i] {
            '%' => result.push('%'),
            'd'|'i' => { result.push_str(&format!("{}", to_int(args.get(arg_i).copied().unwrap_or(NULL)).unwrap_or(0))); arg_i += 1; }
            'u'|'x'|'X'|'o' => {
                let v = to_int(args.get(arg_i).copied().unwrap_or(NULL)).unwrap_or(0) as u64;
                result.push_str(&match chars[i] { 'u'=>format!("{}",v), 'x'=>format!("{:x}",v), 'X'=>format!("{:X}",v), 'o'=>format!("{:o}",v), _=>unreachable!() });
                arg_i += 1;
            }
            'f'|'g'|'e'|'F'|'G'|'E' => {
                result.push_str(&format!("{}", to_double(args.get(arg_i).copied().unwrap_or(NULL))));
                arg_i += 1;
            }
            's' => {
                result.push_str(&String::from_utf8_lossy(&to_lua_string(args.get(arg_i).copied().unwrap_or(NULL))));
                arg_i += 1;
            }
            'c' => { if let Some(c) = to_int(args.get(arg_i).copied().unwrap_or(NULL)) { result.push(c as u8 as char); } arg_i += 1; }
            _ => { result.push('%'); result.push(chars[i]); }
        }
        i += 1;
    }
    rt.intern_string(result.as_bytes())
}

fn do_str_find(args: Args) -> Value {
    let s = match as_string(args[0]) { Some(s) => s, None => return NULL };
    let p = match as_string(args[1]) { Some(p) => p, None => return NULL };
    let init = if args.len() >= 3 { to_int(args[2]).unwrap_or(1) } else { 1 };
    let init = ((init - 1).max(0) as usize).min(s.data.len());
    if p.data.is_empty() { return make_smi((init + 1).min(s.data.len() + 1) as i64); }
    if let Some(pos) = s.data[init..].windows(p.data.len()).position(|w| w == p.data.as_slice()) {
        return make_smi((init + pos + 1) as i64);
    }
    NULL
}

fn do_str_match(rt: &Runtime, args: Args) -> Value {
    let s = match as_string(args[0]) { Some(s) => s, None => return NULL };
    let p = match as_string(args[1]) { Some(p) => p, None => return NULL };
    if p.data.is_empty() { return rt.intern_string(b""); }
    if s.data.windows(p.data.len()).any(|w| w == p.data.as_slice()) {
        return rt.intern_string(&p.data);
    }
    NULL
}

fn do_str_gsub(rt: &Runtime, args: Args) -> Value {
    let s = match as_string(args[0]) { Some(s) => s, None => return NULL };
    let p = match as_string(args[1]) { Some(p) => p, None => return NULL };
    let repl = if is_string(args[2]) { as_string(args[2]).unwrap().data.clone() } else { to_lua_string(args[2]) };
    let s_str = String::from_utf8_lossy(&s.data);
    let p_str = String::from_utf8_lossy(&p.data);
    let r_str = String::from_utf8_lossy(&repl);
    rt.intern_string(s_str.replace(p_str.as_ref(), r_str.as_ref()).as_bytes())
}

// ---- Table functions ----
fn do_tbl_insert(args: Args) -> Value {
    if let Some(t) = as_table(args[0]) {
        let n = t.array_len();
        if args.len() == 2 { t.set(make_smi(n+1), args[1]); }
        else if args.len() >= 3 {
            let pos = to_int(args[1]).unwrap_or(n+1);
            for i in (pos..=n).rev() { t.set(make_smi(i+1), t.get(make_smi(i))); }
            t.set(make_smi(pos), args[2]);
        }
    }
    NULL
}

fn do_tbl_remove(args: Args) -> Value {
    if let Some(t) = as_table(args[0]) {
        let n = t.array_len();
        if n == 0 { return NULL; }
        let pos = if args.len() >= 2 { to_int(args[1]).unwrap_or(n) } else { n };
        let v = t.get(make_smi(pos));
        for i in pos..n { t.set(make_smi(i), t.get(make_smi(i+1))); }
        t.set(make_smi(n), NULL);
        return v;
    }
    NULL
}

fn do_tbl_concat(rt: &Runtime, args: Args) -> Value {
    let t = match as_table(args[0]) { Some(t) => t, None => return rt.intern_string(b"") };
    let sep = if args.len() >= 2 && is_string(args[1]) { as_string(args[1]).unwrap().data.clone() } else { Vec::new() };
    let i_start = if args.len() >= 3 { to_int(args[2]).unwrap_or(1) } else { 1 };
    let i_end = if args.len() >= 4 { to_int(args[3]).unwrap_or(t.array_len()) } else { t.array_len() };
    let mut buf = Vec::new();
    for i in i_start..=i_end {
        if i > i_start { buf.extend_from_slice(&sep); }
        buf.extend_from_slice(&to_lua_string(t.get(make_smi(i))));
    }
    rt.intern_string(&buf)
}

fn do_tbl_sort(args: Args) -> Value {
    if let Some(t) = as_table(args[0]) {
        let n = t.array_len();
        for i in 2..=n {
            let key = t.get(make_smi(i));
            let mut j = i - 1;
            while j >= 1 {
                let cur = t.get(make_smi(j));
                if !(to_double(key) < to_double(cur)) { break; }
                t.set(make_smi(j+1), cur);
                j -= 1;
            }
            t.set(make_smi(j+1), key);
        }
    }
    NULL
}

fn do_tbl_move(args: Args) -> Value {
    if let Some(a1) = as_table(args[0]) {
        let f = to_int(args[1]).unwrap_or(1);
        let e = to_int(args[2]).unwrap_or(0);
        let t_idx = to_int(args[3]).unwrap_or(1);
        let a2 = if args.len() >= 5 && is_table(args[4]) { as_table(args[4]).unwrap() } else { a1 };
        for i in 0..=(e-f) { a2.set(make_smi(t_idx+i), a1.get(make_smi(f+i))); }
    }
    NULL
}

fn do_tbl_pack(rt: &Runtime, args: Args) -> Value {
    let t = LuaTable::new();
    for (i, v) in args.iter().enumerate() { t.set(make_smi((i+1) as i64), *v); }
    t.set(rt.intern_string(b"n"), make_smi(args.len() as i64));
    make_table(t)
}

// ---- Math ----
fn math_unary(args: Args, f64_fn: impl Fn(f64) -> f64, i64_fn: impl Fn(i64) -> i64) -> Value {
    if is_smi(args[0]) { return make_smi_if_fits(i64_fn(smi_value(args[0]))); }
    make_double(f64_fn(to_double(args[0])))
}

fn math_minmax(args: Args, cmp: impl Fn(f64, f64) -> bool) -> Value {
    if args.is_empty() { return NULL; }
    let mut best = to_double(args[0]);
    for v in &args[1..] { let d = to_double(*v); if cmp(d, best) { best = d; } }
    make_smi_if_fits(best as i64)
}

fn do_math_random(args: Args) -> Value {
    use std::time::SystemTime;
    let seed = SystemTime::now().duration_since(SystemTime::UNIX_EPOCH).unwrap_or_default().as_nanos() as u64;
    let r = (seed.wrapping_mul(6364136223846793005).wrapping_add(1442695040888963407) >> 33) as f64 / (1u64 << 31) as f64;
    if args.is_empty() { return make_double(r); }
    let lo = to_int(args[0]).unwrap_or(1);
    let hi = if args.len() >= 2 { to_int(args[1]).unwrap_or(lo) } else { lo };
    let range = (hi - lo + 1) as f64;
    make_smi(lo + (r * range).floor() as i64)
}

// ---- Function call dispatch ----
fn do_call_method(rt: &Runtime, args: Args) -> Value {
    if args.len() < 2 { return NULL; }
    let recv = args[0];
    let name = args[1];
    let method = if is_table(recv) {
        as_table(recv).unwrap().get(name)
    } else if is_string(recv) {
        let strlib_val = rt.globals.get(rt.intern_string(b"string"));
        if is_table(strlib_val) { as_table(strlib_val).unwrap().get(name) } else { NULL }
    } else { NULL };

    if !is_function(method) {
        *rt.error_msg.borrow_mut() = Some("attempt to call method (not a function)".to_string());
        return NULL;
    }
    let mut call_args = Vec::with_capacity(1 + args.len() - 2);
    call_args.push(recv);
    call_args.extend_from_slice(&args[2..]);
    rt.call(method, &call_args)
}

// ---- Scope table helpers ----
fn do_scope_get(rt: &Runtime, args: Args) -> Value {
    if let Some(scope) = as_table(args[0]) {
        let key = args[1];
        let parent_key = rt.intern_string(b"__parent");
        let mut cur = scope;
        for _ in 0..256 {
            let v = cur.get(key);
            if !is_nil(v) { return v; }
            let parent = cur.get(parent_key);
            if !is_table(parent) { break; }
            cur = as_table(parent).unwrap();
        }
        return rt.globals.get(key);
    }
    NULL
}

fn do_scope_set(rt: &Runtime, args: Args) -> Value {
    if let Some(scope) = as_table(args[0]) {
        let key = args[1]; let val = args[2];
        let parent_key = rt.intern_string(b"__parent");
        let mut cur = scope;
        for _ in 0..256 {
            if cur.entries.borrow().contains_key(&ValueKey(key)) {
                cur.set(key, val);
                return NULL;
            }
            let parent = cur.get(parent_key);
            if !is_table(parent) { break; }
            cur = as_table(parent).unwrap();
        }
        rt.globals.set(key, val);
    }
    NULL
}

// ---- Multi-file ----
fn do_require(rt: &Runtime, args: Args) -> Value {
    if let Some(s) = as_string(args[0]) {
        let module_name = s.as_str().to_string();
        if let Some(&v) = rt.modules.borrow().get(&module_name) { return v; }
        let mut found = None;
        for sp in rt.search_paths.borrow().iter() {
            let p = sp.join(format!("{}.lua", module_name));
            if p.exists() { found = Some(p); break; }
        }
        if let Some(path) = found {
            let src = match std::fs::read_to_string(&path) {
                Ok(s) => s,
                Err(e) => { *rt.error_msg.borrow_mut() = Some(format!("cannot read {}: {}", path.display(), e)); return NULL; }
            };
            // SAFETY: require() is called from the dispatch callback which
            // has a &Runtime. We need &mut to run the module. This is safe
            // in practice because the VORTEX interpreter is single-threaded
            // and we're not concurrently accessing the runtime.
            #[allow(invalid_reference_casting)]
            unsafe {
                let rt_mut = &mut *(rt as *const Runtime as *mut Runtime);
                match rt_mut.run_source_named(&path.to_string_lossy(), &src) {
                    Ok(v) => { rt.modules.borrow_mut().insert(module_name, v); return v; }
                    Err(e) => { *rt.error_msg.borrow_mut() = Some(e); return NULL; }
                }
            }
        }
        *rt.error_msg.borrow_mut() = Some(format!("module '{}' not found", module_name));
    }
    NULL
}

fn do_dofile(rt: &Runtime, args: Args) -> Value {
    if let Some(s) = as_string(args[0]) {
        let path = s.as_str().to_string();
        #[allow(invalid_reference_casting)]
        unsafe {
            let rt_mut = &mut *(rt as *const Runtime as *mut Runtime);
            match rt_mut.run_file(&path) {
                Ok(v) => return v,
                Err(e) => { *rt.error_msg.borrow_mut() = Some(e); return NULL; }
            }
        }
    }
    NULL
}

// ---- Registration ----
pub fn register(rt: &Runtime) {
    fn reg(rt: &Runtime, name: &str, fn_ptr: NativeFn) {
        rt.globals.set(rt.intern_string(name.as_bytes()), make_function(LuaFunction::new_native(fn_ptr)));
    }

    // We register native functions that call back into dispatch().
    // This allows us to have a single dispatch path.
    reg(rt, "print", native_print);
    reg(rt, "tostring", native_tostring);
    reg(rt, "tonumber", native_tonumber);
    reg(rt, "type", native_type);
    reg(rt, "assert", native_assert);
    reg(rt, "error", native_error);
    reg(rt, "pcall", native_pcall);
    reg(rt, "select", native_select);
    reg(rt, "rawget", native_rawget);
    reg(rt, "rawset", native_rawset);
    reg(rt, "rawequal", native_rawequal);
    reg(rt, "rawlen", native_rawlen);
    reg(rt, "next", native_next);
    reg(rt, "pairs", native_pairs);
    reg(rt, "ipairs", native_ipairs);
    reg(rt, "unpack", native_unpack);
    reg(rt, "setmetatable", native_setmetatable);
    reg(rt, "getmetatable", native_getmetatable);
    reg(rt, "require", native_require);
    reg(rt, "dofile", native_dofile);

    // Math library
    let math = LuaTable::new();
    let mreg = |t: &Rc<LuaTable>, rt: &Runtime, name: &str, fn_ptr: NativeFn| {
        t.set(rt.intern_string(name.as_bytes()), make_function(LuaFunction::new_native(fn_ptr)));
    };
    mreg(&math, rt, "abs", native_math_abs);
    mreg(&math, rt, "floor", native_math_floor);
    mreg(&math, rt, "ceil", native_math_ceil);
    mreg(&math, rt, "sqrt", native_math_sqrt);
    mreg(&math, rt, "sin", native_math_sin);
    mreg(&math, rt, "cos", native_math_cos);
    mreg(&math, rt, "tan", native_math_tan);
    mreg(&math, rt, "log", native_math_log);
    mreg(&math, rt, "exp", native_math_exp);
    mreg(&math, rt, "pow", native_math_pow);
    mreg(&math, rt, "max", native_math_max);
    mreg(&math, rt, "min", native_math_min);
    mreg(&math, rt, "random", native_math_random);
    math.set(rt.intern_string(b"pi"), make_double(std::f64::consts::PI));
    math.set(rt.intern_string(b"huge"), make_double(f64::INFINITY));
    rt.globals.set(rt.intern_string(b"math"), make_table(math));

    // String library
    let strlib = LuaTable::new();
    mreg(&strlib, rt, "len", native_str_len);
    mreg(&strlib, rt, "sub", native_str_sub);
    mreg(&strlib, rt, "upper", native_str_upper);
    mreg(&strlib, rt, "lower", native_str_lower);
    mreg(&strlib, rt, "rep", native_str_rep);
    mreg(&strlib, rt, "reverse", native_str_reverse);
    mreg(&strlib, rt, "byte", native_str_byte);
    mreg(&strlib, rt, "char", native_str_char);
    mreg(&strlib, rt, "format", native_str_format);
    mreg(&strlib, rt, "find", native_str_find);
    mreg(&strlib, rt, "match", native_str_match);
    mreg(&strlib, rt, "gsub", native_str_gsub);
    rt.globals.set(rt.intern_string(b"string"), make_table(strlib));

    // Table library
    let tablib = LuaTable::new();
    mreg(&tablib, rt, "insert", native_tbl_insert);
    mreg(&tablib, rt, "remove", native_tbl_remove);
    mreg(&tablib, rt, "concat", native_tbl_concat);
    mreg(&tablib, rt, "sort", native_tbl_sort);
    mreg(&tablib, rt, "move", native_tbl_move);
    mreg(&tablib, rt, "pack", native_tbl_pack);
    rt.globals.set(rt.intern_string(b"table"), make_table(tablib));

    // I/O library
    let iolib = LuaTable::new();
    mreg(&iolib, rt, "write", native_io_write);
    mreg(&iolib, rt, "read", native_io_read);
    rt.globals.set(rt.intern_string(b"io"), make_table(iolib));

    // OS library
    let oslib = LuaTable::new();
    mreg(&oslib, rt, "time", native_os_time);
    mreg(&oslib, rt, "clock", native_os_clock);
    mreg(&oslib, rt, "date", native_os_date);
    mreg(&oslib, rt, "getenv", native_os_getenv);
    mreg(&oslib, rt, "execute", native_os_execute);
    mreg(&oslib, rt, "exit", native_os_exit);
    rt.globals.set(rt.intern_string(b"os"), make_table(oslib));

    rt.globals.set(rt.intern_string(b"_VERSION"), rt.intern_string(b"LuaVortex 0.2"));
}

// ---- Native function wrappers ----
// These have the NativeFn signature: fn(&[Value], *mut c_void) -> Value.
// The *mut c_void is the runtime pointer (passed as user_data when the
// function is called via fn_call_method). For direct global calls, it's
// NULL and we use a thread-local.

use std::cell::Cell;
thread_local! { static RT_PTR: Cell<usize> = Cell::new(0); }

fn with_rt<T>(ud: *mut c_void, f: impl FnOnce(&Runtime) -> T) -> T {
    let ptr = if ud.is_null() {
        RT_PTR.with(|c| c.get()) as *const Runtime
    } else {
        ud as *const Runtime
    };
    // SAFETY: the pointer is valid as long as the runtime is alive.
    // This is guaranteed by the Runtime's Drop clearing the thread-local
    // (or the caller passing a valid ud).
    f(unsafe { &*ptr })
}

pub extern "C" fn native_print(args: Args, _ud: *mut c_void) -> Value { do_print(args) }
pub extern "C" fn native_tostring(args: Args, ud: *mut c_void) -> Value {
    with_rt(ud, |rt| rt.intern_string(&to_lua_string(args[0])))
}
pub extern "C" fn native_tonumber(args: Args, _ud: *mut c_void) -> Value { do_tonumber(args) }
pub extern "C" fn native_type(args: Args, ud: *mut c_void) -> Value {
    with_rt(ud, |rt| rt.intern_string(lua_type_name(args[0]).as_bytes()))
}
pub extern "C" fn native_assert(args: Args, ud: *mut c_void) -> Value { with_rt(ud, |rt| do_assert(rt, args)) }
pub extern "C" fn native_error(args: Args, ud: *mut c_void) -> Value { with_rt(ud, |rt| do_error(rt, args)) }
pub extern "C" fn native_pcall(args: Args, _ud: *mut c_void) -> Value { do_select(args); make_bool(true) }
pub extern "C" fn native_select(args: Args, _ud: *mut c_void) -> Value { do_select(args) }
pub extern "C" fn native_rawget(args: Args, _ud: *mut c_void) -> Value { as_table(args[0]).map_or(NULL, |t| t.get(args[1])) }
pub extern "C" fn native_rawset(args: Args, _ud: *mut c_void) -> Value { if let Some(t)=as_table(args[0]) { t.set(args[1], args[2]); } args[0] }
pub extern "C" fn native_rawequal(args: Args, _ud: *mut c_void) -> Value { make_bool(raw_equal(args[0], args[1])) }
pub extern "C" fn native_rawlen(args: Args, _ud: *mut c_void) -> Value { do_rawlen(args) }
pub extern "C" fn native_next(args: Args, _ud: *mut c_void) -> Value {
    as_table(args[0]).map_or(NULL, |t| t.next_key(args.get(1).copied().unwrap_or(UNDEFINED)))
}
pub extern "C" fn native_pairs(args: Args, _ud: *mut c_void) -> Value {
    make_function(LuaFunction::new_native(native_pairs_iter))
}
pub extern "C" fn native_ipairs(args: Args, _ud: *mut c_void) -> Value {
    make_function(LuaFunction::new_native(native_ipairs_iter))
}
pub extern "C" fn native_unpack(args: Args, ud: *mut c_void) -> Value { with_rt(ud, |rt| do_unpack(rt, args)) }
pub extern "C" fn native_setmetatable(args: Args, _ud: *mut c_void) -> Value { do_setmetatable(args) }
pub extern "C" fn native_getmetatable(args: Args, _ud: *mut c_void) -> Value { do_getmetatable(args) }
pub extern "C" fn native_require(args: Args, ud: *mut c_void) -> Value { with_rt(ud, |rt| do_require(rt, args)) }
pub extern "C" fn native_dofile(args: Args, ud: *mut c_void) -> Value { with_rt(ud, |rt| do_dofile(rt, args)) }

// Math natives
pub extern "C" fn native_math_abs(args: Args, _ud: *mut c_void) -> Value { math_unary(args, |a| a.abs(), |a| a.abs()) }
pub extern "C" fn native_math_floor(args: Args, _ud: *mut c_void) -> Value { math_unary(args, |a| a.floor(), |a| a) }
pub extern "C" fn native_math_ceil(args: Args, _ud: *mut c_void) -> Value { math_unary(args, |a| a.ceil(), |a| a) }
pub extern "C" fn native_math_sqrt(args: Args, _ud: *mut c_void) -> Value { make_double(to_double(args[0]).sqrt()) }
pub extern "C" fn native_math_sin(args: Args, _ud: *mut c_void) -> Value { make_double(to_double(args[0]).sin()) }
pub extern "C" fn native_math_cos(args: Args, _ud: *mut c_void) -> Value { make_double(to_double(args[0]).cos()) }
pub extern "C" fn native_math_tan(args: Args, _ud: *mut c_void) -> Value { make_double(to_double(args[0]).tan()) }
pub extern "C" fn native_math_log(args: Args, _ud: *mut c_void) -> Value {
    if args.len() >= 2 { make_double(to_double(args[0]).log(to_double(args[1]))) } else { make_double(to_double(args[0]).ln()) }
}
pub extern "C" fn native_math_exp(args: Args, _ud: *mut c_void) -> Value { make_double(to_double(args[0]).exp()) }
pub extern "C" fn native_math_pow(args: Args, _ud: *mut c_void) -> Value { make_double(to_double(args[0]).powf(to_double(args[1]))) }
pub extern "C" fn native_math_max(args: Args, _ud: *mut c_void) -> Value { math_minmax(args, |a,b| a > b) }
pub extern "C" fn native_math_min(args: Args, _ud: *mut c_void) -> Value { math_minmax(args, |a,b| a < b) }
pub extern "C" fn native_math_random(args: Args, _ud: *mut c_void) -> Value { do_math_random(args) }

// String natives
pub extern "C" fn native_str_len(args: Args, _ud: *mut c_void) -> Value { make_smi(as_string(args[0]).map_or(0, |s| s.len as i64)) }
pub extern "C" fn native_str_sub(args: Args, ud: *mut c_void) -> Value { with_rt(ud, |rt| do_str_sub(rt, args)) }
pub extern "C" fn native_str_upper(args: Args, ud: *mut c_void) -> Value { with_rt(ud, |rt| do_str_upper(rt, args)) }
pub extern "C" fn native_str_lower(args: Args, ud: *mut c_void) -> Value { with_rt(ud, |rt| do_str_lower(rt, args)) }
pub extern "C" fn native_str_rep(args: Args, ud: *mut c_void) -> Value { with_rt(ud, |rt| do_str_rep(rt, args)) }
pub extern "C" fn native_str_reverse(args: Args, ud: *mut c_void) -> Value { with_rt(ud, |rt| do_str_reverse(rt, args)) }
pub extern "C" fn native_str_byte(args: Args, _ud: *mut c_void) -> Value { do_str_byte(args) }
pub extern "C" fn native_str_char(args: Args, ud: *mut c_void) -> Value { with_rt(ud, |rt| do_str_char(rt, args)) }
pub extern "C" fn native_str_format(args: Args, ud: *mut c_void) -> Value { with_rt(ud, |rt| do_str_format(rt, args)) }
pub extern "C" fn native_str_find(args: Args, _ud: *mut c_void) -> Value { do_str_find(args) }
pub extern "C" fn native_str_match(args: Args, ud: *mut c_void) -> Value { with_rt(ud, |rt| do_str_match(rt, args)) }
pub extern "C" fn native_str_gsub(args: Args, ud: *mut c_void) -> Value { with_rt(ud, |rt| do_str_gsub(rt, args)) }

// Table natives
pub extern "C" fn native_tbl_insert(args: Args, _ud: *mut c_void) -> Value { do_tbl_insert(args) }
pub extern "C" fn native_tbl_remove(args: Args, _ud: *mut c_void) -> Value { do_tbl_remove(args) }
pub extern "C" fn native_tbl_concat(args: Args, ud: *mut c_void) -> Value { with_rt(ud, |rt| do_tbl_concat(rt, args)) }
pub extern "C" fn native_tbl_sort(args: Args, _ud: *mut c_void) -> Value { do_tbl_sort(args) }
pub extern "C" fn native_tbl_move(args: Args, _ud: *mut c_void) -> Value { do_tbl_move(args) }
pub extern "C" fn native_tbl_pack(args: Args, ud: *mut c_void) -> Value { with_rt(ud, |rt| do_tbl_pack(rt, args)) }

// I/O natives
pub extern "C" fn native_io_write(args: Args, _ud: *mut c_void) -> Value {
    use std::io::Write;
    let mut o = std::io::stdout().lock();
    for a in args { let _ = o.write_all(&to_lua_string(*a)); }
    NULL
}
pub extern "C" fn native_io_read(args: Args, ud: *mut c_void) -> Value {
    with_rt(ud, |rt| {
        use std::io::BufRead;
        let mut line = String::new();
        let _ = std::io::stdin().lock().read_line(&mut line);
        while line.ends_with('\n') || line.ends_with('\r') { line.pop(); }
        rt.intern_string(line.as_bytes())
    })
}

// OS natives
pub extern "C" fn native_os_time(_args: Args, _ud: *mut c_void) -> Value {
    make_smi(std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).unwrap_or_default().as_secs() as i64)
}
pub extern "C" fn native_os_clock(_args: Args, _ud: *mut c_void) -> Value {
    make_double(std::time::Instant::now().elapsed().as_secs_f64())
}
pub extern "C" fn native_os_date(_args: Args, ud: *mut c_void) -> Value {
    with_rt(ud, |rt| {
        let now = std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).unwrap_or_default().as_secs();
        rt.intern_string(format!("epoch: {}", now).as_bytes())
    })
}
pub extern "C" fn native_os_getenv(args: Args, ud: *mut c_void) -> Value {
    with_rt(ud, |rt| {
        if let Some(s) = as_string(args[0]) {
            if let Ok(val) = std::env::var(s.as_str()) { return rt.intern_string(val.as_bytes()); }
        }
        NULL
    })
}
pub extern "C" fn native_os_execute(args: Args, _ud: *mut c_void) -> Value {
    if let Some(s) = as_string(args[0]) {
        use std::process::Command;
        return make_smi(Command::new("sh").arg("-c").arg(s.as_str()).status().map(|s| s.code().unwrap_or(-1) as i64).unwrap_or(-1));
    }
    make_smi(0)
}
pub extern "C" fn native_os_exit(args: Args, _ud: *mut c_void) -> Value {
    let code = if !args.is_empty() && is_smi(args[0]) { smi_value(args[0]) as i32 } else { 0 };
    std::process::exit(code);
}
