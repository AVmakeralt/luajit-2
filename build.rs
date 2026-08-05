// build.rs — compiles a patched copy of VORTEX's dispatch.c to add the
// extended CALL_RUNTIME callback hook.
//
// The published vortex-jit crate doesn't have the hook. We compile our
// own copy of dispatch.c with the patch applied. The linker uses our
// object file instead of the one in libvortex.a because direct object
// files take precedence over archive members.

use std::path::{Path, PathBuf};

fn main() {
    let vortex_src = find_vortex_src();
    let out_dir = std::env::var("OUT_DIR").unwrap();

    if let Some(ref src) = vortex_src {
        let dispatch_c = src.join("vendor").join("interp").join("dispatch.c");

        if dispatch_c.exists() {
            // Create a patched copy of dispatch.c in OUT_DIR.
            let patched_path = PathBuf::from(&out_dir).join("dispatch_patched.c");
            let contents = std::fs::read_to_string(&dispatch_c).unwrap_or_default();

            if !contents.contains("g_runtime_callback") {
                println!("cargo:warning=Patching VORTEX dispatch.c for extended CALL_RUNTIME");
                let patched = apply_patch(&contents);
                std::fs::write(&patched_path, patched).expect("failed to write patched dispatch.c");
            } else {
                std::fs::copy(&dispatch_c, &patched_path).ok();
            }

            // Compile the patched dispatch.c.
            let mut cc = cc::Build::new();
            cc.compiler("gcc")
                .flag("-std=gnu17")
                .flag("-O2")
                .flag("-fPIC")
                .flag("-fno-strict-aliasing")
                .flag("-DNDEBUG")
                .flag("-Wno-unused-parameter")
                .flag("-Wno-unused-function")
                .flag("-Wno-unused-variable")
                .flag("-Wno-sign-compare")
                .flag("-Wno-discarded-qualifiers")
                .flag("-Wno-incompatible-pointer-types")
                .include(src.join("vendor"))
                .include(&out_dir);

            if let Some(vortex_out_dir) = find_vortex_out_dir() {
                cc.include(&vortex_out_dir);
            }

            cc.file(&patched_path);
            cc.compile("vortex_dispatch_patched");

            // Also compile as a standalone .o and pass via link-arg to
            // override the unpatched version in libvortex.a. The linker
            // processes object files before archives, so our patched
            // dispatch.o will provide vtx_interp_run and the callback
            // functions, and the linker won't pull in the unpatched
            // dispatch.o from libvortex.a.
            let obj_path = PathBuf::from(&out_dir).join("dispatch_patched.o");
            let status = std::process::Command::new("gcc")
                .args(&["-std=gnu17", "-O2", "-fPIC", "-fno-strict-aliasing",
                        "-DNDEBUG", "-c"])
                .arg(format!("-I{}/vendor", src.display()))
                .arg(format!("-I{}", out_dir))
                .arg(if let Some(ref vod) = find_vortex_out_dir() {
                    format!("-I{}", vod.display())
                } else { String::new() })
                .arg(&patched_path)
                .args(&["-o", obj_path.to_str().unwrap()])
                .status();
            if status.map(|s| s.success()).unwrap_or(false) {
                // Use -Wl,--allow-multiple-definition so our patched dispatch.o
                // overrides the unpatched one in libvortex.a.
                println!("cargo:rustc-link-arg=-Wl,--allow-multiple-definition");
                println!("cargo:rustc-link-arg={}", obj_path.display());
            }

            println!("cargo:rerun-if-changed={}", dispatch_c.display());
        }
    } else {
        println!("cargo:warning=Could not locate vortex-jit source");
    }
}

fn find_vortex_src() -> Option<PathBuf> {
    let cargo_home = std::env::var("CARGO_HOME").unwrap_or_else(|_| {
        format!("{}/.cargo", std::env::var("HOME").unwrap_or_else(|_| "/root".to_string()))
    });
    let git_checkouts = PathBuf::from(&cargo_home).join("git").join("checkouts");
    if !git_checkouts.exists() { return None; }
    if let Ok(entries) = std::fs::read_dir(&git_checkouts) {
        for entry in entries.flatten() {
            let name = entry.file_name();
            let name_str = name.to_string_lossy();
            if name_str.starts_with("vortex") || name_str.starts_with("VORTEX") {
                if let Ok(revs) = std::fs::read_dir(entry.path()) {
                    for rev in revs.flatten() {
                        let candidate = rev.path().join("rust-bindings");
                        if candidate.join("vendor/interp/dispatch.c").exists() {
                            return Some(candidate);
                        }
                    }
                }
            }
        }
    }
    None
}

fn find_vortex_out_dir() -> Option<PathBuf> {
    let manifest_dir = std::env::var("CARGO_MANIFEST_DIR").unwrap();
    let target = PathBuf::from(&manifest_dir).join("target");
    for profile in &["debug", "release"] {
        if let Ok(entries) = std::fs::read_dir(target.join(profile).join("build")) {
            for entry in entries.flatten() {
                let name = entry.file_name();
                let name_str = name.to_string_lossy();
                if (name_str.starts_with("vortex-jit") || name_str.starts_with("vortex")) &&
                   entry.path().join("out/vortex_config.h").exists() {
                    return Some(entry.path().join("out"));
                }
            }
        }
    }
    None
}

fn apply_patch(src: &str) -> String {
    let hook_decl = r#"
/* ========================================================================== */
/* LuaVortex extension hook (auto-patched by build.rs)                         */
/* ========================================================================== */
typedef vtx_value_t (*vtx_runtime_callback_t)(uint16_t func_id,
                                                uint16_t arg_count,
                                                const vtx_value_t *argv,
                                                void *user_data);
static _Thread_local vtx_runtime_callback_t g_runtime_callback = NULL;
static _Thread_local void *g_runtime_callback_user_data = NULL;

void vtx_set_runtime_callback(vtx_runtime_callback_t cb, void *user_data) {
    g_runtime_callback = cb;
    g_runtime_callback_user_data = user_data;
}

void vtx_clear_runtime_callback(void) {
    g_runtime_callback = NULL;
    g_runtime_callback_user_data = NULL;
}
"#;

    // Insert after the last #include line.
    let mut insert_pos = 0;
    for (i, line) in src.lines().enumerate() {
        if line.starts_with("#include") {
            insert_pos = src.lines().take(i + 1).map(|l| l.len() + 1).sum::<usize>();
        }
    }

    let mut result = String::with_capacity(src.len() + hook_decl.len() + 1024);
    result.push_str(&src[..insert_pos]);
    result.push_str(hook_decl);
    result.push_str(&src[insert_pos..]);

    // Replace the default case.
    let old_default = r#"        default:
            /* Unknown runtime function — push undefined as a safe fallback */
            *sp++ = VTX_VALUE_UNDEFINED;
            break;
        }
    }
    DISPATCH_NEXT();"#;

    let new_default = r#"        default: {
            /* LuaVortex extended protocol (auto-patched):
             *   operand = (func_id << 6) | arg_count
             * where func_id >= 100. */
            uint16_t lua_fn_id = (uint16_t)(operand >> 6);
            uint16_t lua_argc  = (uint16_t)(operand & 0x3F);
            if (lua_fn_id >= 100 && g_runtime_callback != NULL) {
                vtx_value_t argv_buf[64];
                int argc = lua_argc;
                if (argc > 64) argc = 64;
                for (int i = argc - 1; i >= 0; i--) {
                    argv_buf[i] = *--sp;
                }
                vtx_value_t r = g_runtime_callback(lua_fn_id, lua_argc,
                                                    argv_buf,
                                                    g_runtime_callback_user_data);
                *sp++ = r;
            } else {
                *sp++ = VTX_VALUE_UNDEFINED;
            }
            break;
        }
        }
    }
    DISPATCH_NEXT();"#;

    result = result.replace(old_default, new_default);
    result
}
