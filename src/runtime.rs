//! LuaVortex runtime — wraps the VORTEX runtime and dispatches extended
//! CALL_RUNTIME opcodes to Rust-implemented stdlib functions.

use crate::ast::FuncProto;
use crate::codegen::Compiled;
use crate::stdlib;
use crate::value::*;
use std::os::raw::c_void;
use std::collections::HashMap;
use std::cell::RefCell;
use std::rc::Rc;
use vortex::Runtime as VortexRT;

// The VORTEX bytecode struct layout (must match runtime/bytecode.h).
#[repr(C)]
pub struct VtxBytecode {
    code: *const u8,
    length: usize,
    constant_pool: *mut Value,
    constant_count: u32,
    max_locals: u16,
    max_stack: u16,
}

/// A wrapper around a VORTEX bytecode module constructed in memory.
struct BytecodeBuf {
    ptr: *mut VtxBytecode,
    _code: Vec<u8>,
    _consts: Vec<Value>,
}

impl Drop for BytecodeBuf {
    fn drop(&mut self) {
        unsafe {
            if !self.ptr.is_null() {
                let _ = Box::from_raw(self.ptr);
            }
        }
    }
}

// FFI for the VORTEX runtime's run_with_args.
extern "C" {
    fn vtx_runtime_run_with_args(
        rt: *mut c_void,
        bc: *const VtxBytecode,
        args: *const Value,
        arg_count: u32,
    ) -> Value;
}

/// Lua stdlib function IDs (packed into CALL_RUNTIME operands as
/// (fn_id << 6) | arg_count). Must match codegen.rs.
#[allow(non_camel_case_types)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u16)]
pub enum FnId {
    Print = 100,
    ToString = 101,
    ToNumber = 102,
    Type = 103,
    Assert = 104,
    Error = 105,
    Pcall = 106,
    Select = 107,
    RawGet = 108,
    RawSet = 109,
    RawEqual = 110,
    RawLen = 111,
    Next = 112,
    Pairs = 113,
    IPairs = 114,
    Unpack = 115,
    SetMetatable = 116,
    GetMetatable = 117,
    // Arithmetic
    ArithAdd = 120, ArithSub = 121, ArithMul = 122, ArithDiv = 123,
    ArithIDiv = 124, ArithMod = 125, ArithPow = 126, ArithConcat = 127,
    // Comparisons
    CmpEq = 130, CmpLt = 131, CmpLe = 132,
    // Bitwise
    BitAnd = 140, BitOr = 141, BitXor = 142, BitShl = 143, BitShr = 144,
    // String
    StrLen = 150, StrSub = 151, StrUpper = 152, StrLower = 153,
    StrRep = 154, StrReverse = 155, StrByte = 156, StrChar = 157,
    StrFormat = 158, StrFind = 159,
    StrMatch = 252, StrGsub = 251,
    // Table
    TblInsert = 170, TblRemove = 171, TblConcat = 172, TblSort = 173,
    TblMove = 270, TblPack = 271,
    // Math
    MathAbs = 180, MathFloor = 181, MathCeil = 182, MathSqrt = 183,
    MathSin = 184, MathCos = 185, MathTan = 186, MathLog = 187,
    MathExp = 188, MathPow = 189, MathMax = 190, MathMin = 191,
    MathRandom = 192,
    // I/O
    IoWrite = 200, IoRead = 201,
    // OS
    OsTime = 210, OsClock = 211, OsDate = 212,
    OsGetenv = 280, OsExecute = 281, OsExit = 282,
    // Function call / scope
    Call = 220, CallMethod = 221, NewTable = 222, SetField = 223,
    GetField = 224, NewClosure = 225, Vararg = 226, Length = 227,
    GlobalGet = 228, GlobalSet = 229,
    ScopeGet = 240, ScopeSet = 241, ScopeDeclare = 242,
    NewScope = 243, NewClosureWithEnv = 244,
    // Multi-file
    Require = 300, Dofile = 301,
}

/// The LuaVortex runtime. Wraps a VORTEX runtime and provides the
/// Lua stdlib.
pub struct Runtime {
    pub vrt: VortexRT,
    pub globals: Rc<LuaTable>,
    pub strings: RefCell<Vec<Box<LuaString>>>,
    pub protos: RefCell<Vec<FuncProto>>,
    /// Compiled bytecode buffers, kept alive for the runtime's lifetime.
    /// Keyed by proto_id. The BytecodeBuf owns the code/constant Vecs.
    pub proto_bytecode: RefCell<HashMap<i32, Box<BytecodeBuf>>>,
    pub modules: RefCell<HashMap<String, Value>>,
    pub error_msg: RefCell<Option<String>>,
    pub search_paths: RefCell<Vec<std::path::PathBuf>>,
}

unsafe impl Send for Runtime {}

impl Runtime {
    pub fn new() -> Result<Self, String> {
        let vrt = VortexRT::new()?;
        let globals = LuaTable::new();
        let rt = Runtime {
            vrt,
            globals,
            strings: RefCell::new(Vec::new()),
            protos: RefCell::new(Vec::new()),
            proto_bytecode: RefCell::new(HashMap::new()),
            modules: RefCell::new(HashMap::new()),
            error_msg: RefCell::new(None),
            search_paths: RefCell::new(vec![
                std::path::PathBuf::from("."),
                std::path::PathBuf::from("./lib"),
            ]),
        };
        stdlib::register(&rt);
        Ok(rt)
    }

    /// Run a Lua source string.
    pub fn run_source(&mut self, src: &str) -> Result<Value, String> {
        self.run_source_named("(string)", src)
    }

    /// Run a Lua source string with a name (for error messages).
    pub fn run_source_named(&mut self, name: &str, src: &str) -> Result<Value, String> {
        let mut parser = crate::parser::Parser::new(src, name);
        let chunk = parser.parse().ok_or_else(|| {
            parser.last_error.clone().unwrap_or_else(|| "unknown parse error".to_string())
        })?;
        let compiled = crate::codegen::compile(self, &chunk)
            .map_err(|e| format!("compile error: {}", e))?;

        // Build the bytecode struct and keep the buffers alive
        let bc = Box::new(VtxBytecode {
            code: compiled.code.as_ptr(),
            length: compiled.code.len(),
            constant_pool: compiled.constants.as_ptr() as *mut Value,
            constant_count: compiled.constants.len() as u32,
            max_locals: compiled.max_locals,
            max_stack: compiled.max_stack,
        });
        let bc_ptr = Box::into_raw(bc);
        // Store the buf so the code/constants Vecs stay alive
        self.proto_bytecode.borrow_mut().insert(-1, Box::new(BytecodeBuf {
            ptr: bc_ptr,
            _code: compiled.code,
            _consts: compiled.constants,
        }));

        // Register the runtime callback
        let rt_ptr = self as *mut Runtime as *mut c_void;
        vortex::set_runtime_callback(Some(dispatch_callback), rt_ptr);

        let env_val = make_table(self.globals.clone());
        let vrt_ptr = self.vrt.as_ptr() as *mut c_void;
        let result = unsafe {
            vtx_runtime_run_with_args(
                vrt_ptr,
                bc_ptr,
                &[env_val] as *const Value,
                1,
            )
        };

        vortex::set_runtime_callback(None, std::ptr::null_mut());

        // Clean up the main chunk bytecode
        self.proto_bytecode.borrow_mut().remove(&-1);

        if let Some(e) = self.error_msg.borrow_mut().take() {
            return Err(e);
        }
        Ok(result)
    }

    /// Run a Lua file.
    pub fn run_file(&mut self, path: impl AsRef<std::path::Path>) -> Result<Value, String> {
        let path = path.as_ref();
        let src = std::fs::read_to_string(path).map_err(|e| format!("cannot read {}: {}", path.display(), e))?;
        self.run_source_named(&path.to_string_lossy(), &src)
    }

    /// Intern a string. Returns a NaN-boxed heap pointer Value.
    pub fn intern_string(&self, data: &[u8]) -> Value {
        for s in self.strings.borrow().iter() {
            if s.data == data {
                unsafe { return make_heap_ptr(s.as_ref() as *const LuaString as *mut c_void); }
            }
        }
        let s = LuaString::new(data.to_vec());
        let ptr = unsafe { make_heap_ptr(Box::into_raw(s) as *mut c_void) };
        unsafe {
            let raw = heap_ptr(ptr);
            let boxed = Box::from_raw(raw as *mut LuaString);
            self.strings.borrow_mut().push(boxed);
        }
        ptr
    }

    /// Register a function proto. Returns a unique ID.
    pub fn register_proto(&self, proto: FuncProto) -> i32 {
        let mut protos = self.protos.borrow_mut();
        let id = protos.len() as i32;
        protos.push(proto);
        id
    }

    /// Associate compiled bytecode with a proto ID.
    pub fn set_proto_bytecode(&self, proto_id: i32, compiled: Compiled) {
        let bc = Box::new(VtxBytecode {
            code: compiled.code.as_ptr(),
            length: compiled.code.len(),
            constant_pool: compiled.constants.as_ptr() as *mut Value,
            constant_count: compiled.constants.len() as u32,
            max_locals: compiled.max_locals,
            max_stack: compiled.max_stack,
        });
        let bc_ptr = Box::into_raw(bc);
        let buf = BytecodeBuf {
            ptr: bc_ptr,
            _code: compiled.code,
            _consts: compiled.constants,
        };
        self.proto_bytecode.borrow_mut().insert(proto_id, Box::new(buf));
    }

    /// Get the bytecode pointer for a proto.
    pub fn get_proto_bytecode(&self, proto_id: i32) -> Option<*mut VtxBytecode> {
        self.proto_bytecode.borrow().get(&proto_id).map(|b| b.ptr)
    }

    /// Get a proto by ID.
    pub fn get_proto(&self, proto_id: i32) -> Option<FuncProto> {
        let protos = self.protos.borrow();
        if proto_id >= 0 && (proto_id as usize) < protos.len() {
            Some(protos[proto_id as usize].clone())
        } else {
            None
        }
    }

    /// Call a Lua function value with the given arguments.
    ///
    /// This takes `&self` (not `&mut self`) because it's called from
    /// the dispatch callback which only has a shared reference. The
    /// VORTEX runtime's `run_with_args` is called via FFI with a raw
    /// pointer, which is safe in practice (the VORTEX runtime is
    /// single-threaded per call).
    pub fn call(&self, fn_val: Value, args: &[Value]) -> Value {
        let f = match as_function(fn_val) {
            Some(f) => f,
            None => {
                *self.error_msg.borrow_mut() = Some(format!("attempt to call a {} value", lua_type_name(fn_val)));
                return NULL;
            }
        };

        if let Some(native_fn) = f.native {
            let rt_ptr = self as *const Runtime as *mut c_void;
            return native_fn(args, rt_ptr);
        }

        let proto_id = f.proto_id;
        if proto_id < 0 {
            *self.error_msg.borrow_mut() = Some(format!("invalid proto id {}", proto_id));
            return NULL;
        }

        let bc_ptr = match self.get_proto_bytecode(proto_id) {
            Some(p) => p,
            None => {
                *self.error_msg.borrow_mut() = Some(format!("no bytecode for proto {}", proto_id));
                return NULL;
            }
        };

        let proto = match self.get_proto(proto_id) {
            Some(p) => p,
            None => {
                *self.error_msg.borrow_mut() = Some(format!("no proto {}", proto_id));
                return NULL;
            }
        };

        let scope = LuaTable::new();
        let parent_val = f.captured_env.unwrap_or_else(|| make_table(self.globals.clone()));
        let parent_key = self.intern_string(b"__parent");
        scope.set(parent_key, parent_val);

        for (i, param) in proto.params.iter().enumerate() {
            let pname = self.intern_string(param.as_bytes());
            let val = args.get(i).copied().unwrap_or(NULL);
            scope.set(pname, val);
        }

        let scope_val = make_table(scope);
        let rt_ptr = self as *const Runtime as *mut c_void;
        // Get the VORTEX runtime pointer. The vortex::Runtime struct
        // has `inner: ffi::vtx_runtime_t` as its only field. A pointer
        // to the vortex::Runtime IS a pointer to the inner runtime
        // (same address, since it's the first and only field).
        let vrt_ptr = &self.vrt as *const VortexRT as *mut c_void;
        vortex::set_runtime_callback(Some(dispatch_callback), rt_ptr);
        unsafe {
            let result = vtx_runtime_run_with_args(
                vrt_ptr,
                bc_ptr,
                &[scope_val] as *const Value,
                1,
            );
            vortex::set_runtime_callback(None, std::ptr::null_mut());
            result
        }
    }

    /// Add a search path for require().
    pub fn add_search_path(&self, path: impl Into<std::path::PathBuf>) {
        self.search_paths.borrow_mut().push(path.into());
    }
}

/// The C-callable callback that dispatches extended CALL_RUNTIME opcodes.
///
/// VORTEX 0.7.1+ callback signature: `(operand, sp, user_data) -> i32`
///   - operand: the CALL_RUNTIME operand = (fn_id << 6) | argc
///   - sp: pointer to the stack pointer; the callback pops args and pushes results
///   - returns: number of values pushed (0 = void, 1 = single)
extern "C" fn dispatch_callback(
    operand: u32,
    sp: *mut *mut u64,
    user_data: *mut c_void,
) -> i32 {
    let rt: &Runtime = unsafe { &*(user_data as *const Runtime) };
    let fn_id = (operand >> 6) as u16;
    let argc = (operand & 0x3F) as usize;

    // Pop `argc` values from the stack into an argv buffer.
    let mut argv_buf = [0u64; 64];
    if argc > 0 && argc <= 64 {
        unsafe {
            let stack_ptr = *sp;
            for i in 0..argc {
                argv_buf[i] = *stack_ptr.sub(argc - i);
            }
            *sp = stack_ptr.sub(argc);
        }
    }

    let args = &argv_buf[..argc.min(64)];
    let result = stdlib::dispatch(rt, fn_id, args);

    // Push the result onto the stack
    unsafe {
        let stack_ptr = *sp;
        *stack_ptr = result;
        *sp = stack_ptr.add(1);
    }

    1 // pushed 1 value
}
