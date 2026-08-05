//! Lua value model on top of VORTEX's NaN-boxing.
//!
//! Lua values are represented as `vortex::Value` (u64) with the following
//! mapping:
//!
//! | Lua type   | VORTEX representation                |
//! |------------|--------------------------------------|
//! | nil        | VORTEX null                          |
//! | true/false | VORTEX true/false                    |
//! | integer    | SMI (48-bit signed, NaN-boxed)       |
//! | float      | raw IEEE 754 double                  |
//! | string     | heap pointer to `LuaString`          |
//! | table      | heap pointer to `LuaTable`           |
//! | function   | heap pointer to `LuaFunction`        |

use std::os::raw::c_void;
use std::collections::HashMap;
use std::cell::RefCell;
use std::rc::Rc;

pub type Value = u64;

// VORTEX NaN-boxing constants
const HEADER: u64 = 0x7FF8_0000_0000_0000;
const TAG_MASK: u64 = 0x0007;
const TAG_SMI: u64 = 0;
const TAG_HEAP: u64 = 1;
const TAG_DOUBLE: u64 = 2;
const TAG_BOOL: u64 = 3;
const TAG_NULL: u64 = 4;
const TAG_UNDEFINED: u64 = 5;
const DATA_SHIFT: u32 = 3;
const DATA_MASK: u64 = 0x0000_FFFF_FFFF_FFFF;

pub const NULL: Value = HEADER | TAG_NULL;
pub const UNDEFINED: Value = HEADER | TAG_UNDEFINED;
pub const TRUE: Value = HEADER | (1 << DATA_SHIFT) | TAG_BOOL;
pub const FALSE: Value = HEADER | TAG_BOOL;

// SMI range: 46-bit signed
pub const SMI_MIN: i64 = -(1i64 << 45);
pub const SMI_MAX: i64 = (1i64 << 45) - 1;

// ---- NaN-boxing helpers ----

pub fn make_smi(val: i64) -> Value {
    debug_assert!(val >= SMI_MIN && val <= SMI_MAX, "SMI out of range: {}", val);
    let raw = (val as u64) & DATA_MASK;
    HEADER | (raw << DATA_SHIFT) | TAG_SMI
}

pub fn smi_value(val: Value) -> i64 {
    let raw = (val >> DATA_SHIFT) & DATA_MASK;
    // Sign-extend from 48 bits
    if raw & (1 << 47) != 0 {
        (raw | !DATA_MASK) as i64
    } else {
        raw as i64
    }
}

pub fn is_smi(val: Value) -> bool {
    (val & HEADER) == HEADER && (val & TAG_MASK) == TAG_SMI
}

pub fn make_double(val: f64) -> Value {
    let bits = val.to_bits();
    // Check for NaN
    if ((bits >> 52) & 0x7FF) == 0x7FF && (bits & 0x000F_FFFF_FFFF_FFFF) != 0 {
        return HEADER | TAG_DOUBLE;
    }
    bits
}

pub fn double_value(val: Value) -> f64 {
    if (val & HEADER) != HEADER {
        return f64::from_bits(val);
    }
    f64::from_bits(HEADER) // canonical NaN
}

pub fn is_double(val: Value) -> bool {
    if (val & HEADER) != HEADER {
        return true; // raw non-NaN double
    }
    (val & TAG_MASK) == TAG_DOUBLE
}

pub unsafe fn make_heap_ptr(ptr: *mut c_void) -> Value {
    let p = ptr as u64;
    let raw = (p >> DATA_SHIFT) & DATA_MASK;
    HEADER | (raw << DATA_SHIFT) | TAG_HEAP
}

pub unsafe fn heap_ptr(val: Value) -> *mut c_void {
    let raw = (val >> DATA_SHIFT) & DATA_MASK;
    (raw << DATA_SHIFT) as *mut c_void
}

pub fn is_heap_ptr(val: Value) -> bool {
    (val & HEADER) == HEADER && (val & TAG_MASK) == TAG_HEAP
}

pub fn make_bool(b: bool) -> Value {
    if b { TRUE } else { FALSE }
}

pub fn is_bool(val: Value) -> bool {
    (val & HEADER) == HEADER && (val & TAG_MASK) == TAG_BOOL
}

pub fn bool_value(val: Value) -> bool {
    val != FALSE
}

pub fn is_null(val: Value) -> bool {
    val == NULL
}

pub fn is_undefined(val: Value) -> bool {
    val == UNDEFINED
}

/// Kind tag for Lua heap objects (stored in the first field).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u32)]
pub enum LuaKind {
    String = 1,
    Table = 2,
    Function = 3,
}

/// A Lua string. Stored as a heap object with a kind tag.
#[repr(C)]
pub struct LuaString {
    pub kind: u32,
    pub _mark: u32,
    pub len: usize,
    pub data: Vec<u8>,
}

impl LuaString {
    pub fn new(data: Vec<u8>) -> Box<Self> {
        Box::new(LuaString {
            kind: LuaKind::String as u32,
            _mark: 0,
            len: data.len(),
            data,
        })
    }

    pub fn as_str(&self) -> &str {
        std::str::from_utf8(&self.data).unwrap_or("<invalid utf8>")
    }
}

/// A Lua table entry.
#[derive(Clone)]
pub struct TableEntry {
    pub key: Value,
    pub value: Value,
}

/// A Lua table (hash map with integer key optimization).
#[repr(C)]
pub struct LuaTable {
    pub kind: u32,
    pub _mark: u32,
    pub entries: RefCell<HashMap<ValueKey, Value>>,
    pub metatable: RefCell<Option<Rc<LuaTable>>>,
}

/// A key wrapper that implements Hash/Eq for Value.
/// NaN and nil cannot be keys; everything else is hashed by its bits.
#[derive(Debug, Clone, Copy)]
pub struct ValueKey(pub Value);

impl std::hash::Hash for ValueKey {
    fn hash<H: std::hash::Hasher>(&self, state: &mut H) {
        // For numbers, normalize -0.0 to 0.0.
        if is_double(self.0) {
            let d = double_value(self.0);
            if d == 0.0 {
                0.0f64.to_bits().hash(state);
            } else {
                self.0.hash(state);
            }
        } else {
            self.0.hash(state);
        }
    }
}

impl PartialEq for ValueKey {
    fn eq(&self, other: &Self) -> bool {
        raw_equal(self.0, other.0)
    }
}

impl Eq for ValueKey {}

impl LuaTable {
    pub fn new() -> Rc<Self> {
        Rc::new(LuaTable {
            kind: LuaKind::Table as u32,
            _mark: 0,
            entries: RefCell::new(HashMap::new()),
            metatable: RefCell::new(None),
        })
    }

    pub fn get(&self, key: Value) -> Value {
        if is_nil(key) { return NULL; }
        self.entries.borrow().get(&ValueKey(key)).copied().unwrap_or(NULL)
    }

    pub fn set(&self, key: Value, value: Value) {
        if is_nil(key) { return; }
        if is_nil(value) {
            self.entries.borrow_mut().remove(&ValueKey(key));
        } else {
            self.entries.borrow_mut().insert(ValueKey(key), value);
        }
    }

    pub fn len(&self) -> usize {
        self.entries.borrow().len()
    }

    pub fn array_len(&self) -> i64 {
        // Linear scan for the array part length (1..n where t[n+1] is nil).
        let mut n = 0i64;
        loop {
            let v = self.get(make_smi(n + 1));
            if is_nil(v) { break; }
            n += 1;
            if n > (1 << 40) { break; }
        }
        n
    }

    pub fn next_key(&self, prev: Value) -> Value {
        let entries = self.entries.borrow();
        if is_nil(prev) || is_undefined(prev) {
            // Return the first key.
            if let Some((&k, _)) = entries.iter().next() {
                return k.0;
            }
            return NULL;
        }
        // Find prev in the map, return the next key.
        let mut found = false;
        for (&k, _) in entries.iter() {
            if found {
                return k.0;
            }
            if raw_equal(k.0, prev) {
                found = true;
            }
        }
        NULL
    }
}

/// A Lua function (either a closure with a proto_id or a native Rust function).
#[repr(C)]
pub struct LuaFunction {
    pub kind: u32,
    pub _mark: u32,
    pub proto_id: i32,
    pub native: Option<NativeFn>,
    pub captured_env: Option<Value>,
    pub compiled_bc: Option<*mut c_void>,
    /// Cached param count — avoids RefCell borrow + Vec lookup on every call.
    pub nparams: u32,
}

/// Native function type: takes (args, rt_ptr) and returns a Value.
/// The rt_ptr is a *mut c_void pointing to the Runtime (or null when
/// called directly from bytecode dispatch, in which case the thread-local
/// RT_PTR is used).
pub type NativeFn = extern "C" fn(&[Value], *mut c_void) -> Value;

impl LuaFunction {
    pub fn new_lua(proto_id: i32) -> Box<Self> {
        Box::new(LuaFunction {
            kind: LuaKind::Function as u32,
            _mark: 0,
            proto_id,
            native: None,
            captured_env: None,
            compiled_bc: None,
            nparams: 0,
        })
    }

    pub fn new_native(fn_ptr: NativeFn) -> Box<Self> {
        Box::new(LuaFunction {
            kind: LuaKind::Function as u32,
            _mark: 0,
            proto_id: -1,
            native: Some(fn_ptr),
            captured_env: None,
            compiled_bc: None,
            nparams: 0,
        })
    }
}

// ---- Value helpers ----

pub fn is_nil(v: Value) -> bool {
    is_null(v) || is_undefined(v)
}

pub fn is_truthy(v: Value) -> bool {
    if is_nil(v) { return false; }
    if is_bool(v) { return bool_value(v); }
    true
}

pub fn lua_type_name(v: Value) -> &'static str {
    if is_nil(v) { "nil" }
    else if is_bool(v) { "boolean" }
    else if is_smi(v) || is_double(v) { "number" }
    else if is_string(v) { "string" }
    else if is_table(v) { "table" }
    else if is_function(v) { "function" }
    else { "unknown" }
}

pub fn is_string(v: Value) -> bool {
    lua_kind(v) == Some(LuaKind::String)
}

pub fn is_table(v: Value) -> bool {
    lua_kind(v) == Some(LuaKind::Table)
}

pub fn is_function(v: Value) -> bool {
    lua_kind(v) == Some(LuaKind::Function)
}

pub fn lua_kind(v: Value) -> Option<LuaKind> {
    if !is_heap_ptr(v) { return None; }
    unsafe {
        let ptr = heap_ptr(v) as *const u32;
        if ptr.is_null() { return None; }
        match *ptr {
            1 => Some(LuaKind::String),
            2 => Some(LuaKind::Table),
            3 => Some(LuaKind::Function),
            _ => None,
        }
    }
}

/// Get a LuaString reference from a value.
///
/// # Safety
/// The returned reference is valid as long as the LuaString is alive
/// (i.e., as long as the Runtime that owns it is alive).
pub fn as_string<'a>(v: Value) -> Option<&'a LuaString> {
    if !is_string(v) { return None; }
    unsafe { (heap_ptr(v) as *const LuaString).as_ref() }
}

/// Get a LuaTable reference from a value.
///
/// # Safety
/// The returned reference is valid as long as the LuaTable is alive.
pub fn as_table<'a>(v: Value) -> Option<&'a LuaTable> {
    if !is_table(v) { return None; }
    unsafe { (heap_ptr(v) as *const LuaTable).as_ref() }
}

/// Get a LuaFunction reference from a value.
///
/// # Safety
/// The returned reference is valid as long as the LuaFunction is alive.
pub fn as_function<'a>(v: Value) -> Option<&'a LuaFunction> {
    if !is_function(v) { return None; }
    unsafe { (heap_ptr(v) as *const LuaFunction).as_ref() }
}

/// Wrap a boxed LuaString into a Value.
pub fn make_string(s: Box<LuaString>) -> Value {
    unsafe { make_heap_ptr(Box::into_raw(s) as *mut c_void) }
}

/// Wrap a boxed LuaTable into a Value.
pub fn make_table(t: Rc<LuaTable>) -> Value {
    // Rc doesn't give us a raw pointer we can safely store. We leak the Rc
    // to get a static reference. (GC integration is future work.)
    unsafe { make_heap_ptr(Rc::into_raw(t) as *mut c_void) }
}

/// Wrap a boxed LuaFunction into a Value.
pub fn make_function(f: Box<LuaFunction>) -> Value {
    unsafe { make_heap_ptr(Box::into_raw(f) as *mut c_void) }
}

/// Convert any value to a Lua-style string (owned Vec<u8>).
pub fn to_lua_string(v: Value) -> Vec<u8> {
    if is_nil(v) { return b"nil".to_vec(); }
    if is_bool(v) { return if bool_value(v) { b"true".to_vec() } else { b"false".to_vec() }; }
    if is_smi(v) { return format!("{}", smi_value(v)).into_bytes(); }
    if is_double(v) {
        let d = double_value(v);
        if d.fract() == 0.0 && d.abs() < 1e15 {
            return format!("{}", d as i64).into_bytes();
        }
        return format!("{}", d).into_bytes();
    }
    if let Some(s) = as_string(v) { return s.data.clone(); }
    if is_table(v) {
        return format!("table: 0x{:x}", v).into_bytes();
    }
    if is_function(v) {
        return format!("function: 0x{:x}", v).into_bytes();
    }
    b"<unknown>".to_vec()
}

/// Raw equality (Lua's == without metamethods).
pub fn raw_equal(a: Value, b: Value) -> bool {
    if a == b { return true; }
    // Numbers: compare by value.
    if (is_smi(a) || is_double(a)) && (is_smi(b) || is_double(b)) {
        let da = if is_smi(a) { smi_value(a) as f64 } else { double_value(a) };
        let db = if is_smi(b) { smi_value(b) as f64 } else { double_value(b) };
        if da.is_nan() || db.is_nan() { return false; }
        return da == db;
    }
    // Strings: compare by content.
    if is_string(a) && is_string(b) {
        let sa = as_string(a).unwrap();
        let sb = as_string(b).unwrap();
        return sa.data == sb.data;
    }
    false
}
