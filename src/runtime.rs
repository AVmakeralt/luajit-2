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

// The VORTEX runtime struct — we allocate it directly on the heap to
// avoid move-related dangling pointer issues (the runtime has
// self-referential pointers like interp->gc = &rt->gc).
#[repr(C)]
pub struct VtxRuntime {
    type_system: [u8; 512],  // vtx_type_system_t (oversized to be safe)
    gc: [u8; 512],           // vtx_gc_t (oversized)
    interp: *mut c_void,
    // ... more fields follow, but we only need the pointer
}

extern "C" {
    fn vtx_runtime_create(rt: *mut c_void) -> i32;
    fn vtx_runtime_destroy(rt: *mut c_void);
    fn vtx_runtime_enable_jit(rt: *mut c_void, nthreads: u32);
    fn vtx_runtime_run_with_args(
        rt: *mut c_void,
        bc: *const VtxBytecode,
        args: *const Value,
        arg_count: u32,
    ) -> Value;
    fn vtx_runtime_interp(rt: *mut c_void) -> *mut c_void;
}

/// The VORTEX interpreter struct (partial — we only need running and current_frame).
/// Field offsets must match vtx_interp_t in interp/dispatch.h.
#[repr(C)]
struct VtxInterpState {
    _frame_stack: [u8; 64],  // vtx_frame_stack_t (oversized)
    current_frame: *mut c_void,
    // ... more fields between current_frame and running ...
    // We'll compute the running offset dynamically
}

/// After a re-entrant `vtx_runtime_run_with_args` call (from within a
/// callback), the function's `vtx_interp_run` sets `interp->running = false`
/// and `interp->current_frame = NULL`. This corrupts the main chunk's
/// interpreter state. We restore `running = true` after the re-entrant call.
///
/// The `running` field offset in `vtx_interp_t` is computed from the FFI
/// bindings' struct layout:
///   frame_stack(24) + current_frame(8) + profiler(144) + type_feedback(48)
///   + type_system(8) + gc(8) + compile_ctx(8) + dispatch_table(8) = 256
const INTERP_RUNNING_OFFSET: usize = 256;

unsafe fn restore_interp_running(vrt_ptr: *mut c_void) {
    let interp = vtx_runtime_interp(vrt_ptr);
    if !interp.is_null() {
        *(interp as *mut u8).add(INTERP_RUNNING_OFFSET) = 1; // running = true
    }
}

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
    /// Heap-allocated VORTEX runtime. MUST NOT move after initialization
    /// because it contains self-referential pointers (interp->gc = &rt->gc).
    vrt_ptr: *mut c_void,
    pub globals: Rc<LuaTable>,
    pub strings: RefCell<Vec<Box<LuaString>>>,
    pub protos: RefCell<Vec<FuncProto>>,
    pub proto_bytecode: RefCell<HashMap<i32, Box<BytecodeBuf>>>,
    pub modules: RefCell<HashMap<String, Value>>,
    pub error_msg: RefCell<Option<String>>,
    pub search_paths: RefCell<Vec<std::path::PathBuf>>,
}

unsafe impl Send for Runtime {}

impl Runtime {
    pub fn new() -> Result<Self, String> {
        // Allocate the VORTEX runtime on the heap so it never moves.
        // The vtx_runtime_t struct is large (~several KB). We allocate
        // via Box<[u8]> with a generous size, then cast.
        // The actual size doesn't matter as long as it's large enough —
        // vtx_runtime_create will initialize the fields.
        let layout = std::alloc::Layout::from_size_align(16384, 8).unwrap();
        let vrt_ptr = unsafe { std::alloc::alloc_zeroed(layout) as *mut c_void };
        if vrt_ptr.is_null() {
            return Err("failed to allocate VORTEX runtime".to_string());
        }
        let rc = unsafe { vtx_runtime_create(vrt_ptr) };
        if rc != 0 {
            unsafe { std::alloc::dealloc(vrt_ptr as *mut u8, layout); }
            return Err("failed to create VORTEX runtime".to_string());
        }

        let globals = LuaTable::new();
        let rt = Runtime {
            vrt_ptr,
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

    /// Enable JIT compilation.
    pub fn enable_jit(&mut self, nthreads: u32) {
        unsafe { vtx_runtime_enable_jit(self.vrt_ptr, nthreads); }
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
        let vrt_ptr = self.vrt_ptr;
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

        // Build the args array: [env_table, param1, param2, ...]
        // local 0 = env (globals table), local 1..n = parameters
        let env_val = make_table(self.globals.clone());
        let mut run_args = Vec::with_capacity(1 + proto.params.len());
        run_args.push(env_val);
        for i in 0..proto.params.len() {
            run_args.push(args.get(i).copied().unwrap_or(NULL));
        }

        let rt_ptr = self as *const Runtime as *mut c_void;
        let vrt_ptr = self.vrt_ptr;
        vortex::set_runtime_callback(Some(dispatch_callback), rt_ptr);
        let result = unsafe {
            let r = vtx_runtime_run_with_args(
                vrt_ptr,
                bc_ptr,
                run_args.as_ptr(),
                run_args.len() as u32,
            );
            // Restore interpreter state corrupted by the re-entrant call.
            // vtx_interp_run sets interp->running = false on return, which
            // stops the main chunk's interpreter. We set it back to true.
            restore_interp_running(vrt_ptr);
            r
        };
        // Re-register the callback for the main chunk's continued execution
        vortex::set_runtime_callback(Some(dispatch_callback), rt_ptr);
        result
    }

    /// Add a search path for require().
    pub fn add_search_path(&self, path: impl Into<std::path::PathBuf>) {
        self.search_paths.borrow_mut().push(path.into());
    }
}

impl Drop for Runtime {
    fn drop(&mut self) {
        unsafe {
            vtx_runtime_destroy(self.vrt_ptr);
            let layout = std::alloc::Layout::from_size_align(16384, 8).unwrap();
            std::alloc::dealloc(self.vrt_ptr as *mut u8, layout);
        }
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
                argv_buf[i] = *stack_ptr.wrapping_sub(argc - i);
            }
            *sp = stack_ptr.wrapping_sub(argc);
        }
    }

    let args = &argv_buf[..argc.min(64)];
    let result = stdlib::dispatch(rt, fn_id, args);

    // Push the result onto the stack
    unsafe {
        let stack_ptr = *sp;
        *stack_ptr = result;
        *sp = stack_ptr.wrapping_add(1);
    }

    1 // pushed 1 value
}
