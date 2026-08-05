use luavortex::Runtime;
use std::process;

fn main() {
    let args: Vec<String> = std::env::args().collect();
    let mut prog = &args[0];

    let mut exec_str: Option<String> = None;
    let mut file: Option<String> = None;
    let mut interactive = false;
    let mut use_jit = true;
    let mut tier: u32 = 0;

    let mut i = 1;
    while i < args.len() {
        let a = &args[i];
        match a.as_str() {
            "-e" => {
                i += 1;
                if i >= args.len() { usage(prog); process::exit(2); }
                exec_str = Some(args[i].clone());
            }
            "-i" => interactive = true,
            "--no-jit" => use_jit = false,
            "--tier" => {
                i += 1;
                if i >= args.len() { usage(prog); process::exit(2); }
                tier = args[i].parse().unwrap_or(0);
            }
            "--help" | "-h" => { usage(prog); process::exit(0); }
            "--version" => {
                println!("LuaVortex 0.2.0 (Rust)");
                println!("Built on VORTEX JIT runtime");
                process::exit(0);
            }
            _ if a.starts_with('-') && a.len() > 1 => {
                eprintln!("luavortex: unknown option '{}'", a);
                usage(prog);
                process::exit(2);
            }
            _ => {
                file = Some(a.clone());
                i += 1;
                break;
            }
        }
        i += 1;
    }

    let mut rt = match Runtime::new() {
        Ok(rt) => rt,
        Err(e) => {
            eprintln!("luavortex: failed to create runtime: {}", e);
            process::exit(1);
        }
    };

    if use_jit {
        rt.enable_jit(2);
    }

    let mut rc = 0;

    if let Some(src) = exec_str {
        if let Err(e) = rt.run_source_named("(command line)", &src) {
            eprintln!("luavortex: {}", e);
            rc = 1;
        }
    } else if let Some(path) = file {
        if let Err(e) = rt.run_file(&path) {
            eprintln!("luavortex: {}", e);
            rc = 1;
        }
    } else if !interactive {
        usage(prog);
        rc = 2;
    }

    if interactive {
        repl(&mut rt);
    }

    // Eager compilation at requested tier
    if tier > 0 {
        // The bytecode is already compiled; tier compilation would happen
        // via rt.vrt.compile_method(). This is a no-op for now since we
        // don't have a handle to the main bytecode.
    }

    process::exit(rc);
}

fn repl(rt: &mut Runtime) {
    use std::io::{Write, BufRead};
    println!("LuaVortex 0.2.0 — type 'exit' to quit");
    let stdin = std::io::stdin();
    loop {
        print!("> ");
        let _ = std::io::stdout().flush();
        let mut line = String::new();
        if stdin.lock().read_line(&mut line).is_err() {
            break;
        }
        let line = line.trim();
        if line == "exit" { break; }
        if line.is_empty() { continue; }
        if let Err(e) = rt.run_source_named("(repl)", line) {
            eprintln!("error: {}", e);
        }
    }
}

fn usage(prog: &str) {
    eprintln!(
        "LuaVortex 0.2.0 — a Lua 5.4 frontend for VORTEX JIT (Rust)\n\
         \n\
         Usage: {} [options] [file.lua [args...]]\n\
                {} -e 'code'\n\
         \n\
         Options:\n\
           -e CODE     Execute CODE as a Lua chunk\n\
           -i          Interactive REPL\n\
           --no-jit    Disable VORTEX JIT (interpreter only)\n\
           --tier N    Eagerly compile at tier N (1 or 2)\n\
           --help      Show this help\n\
           --version   Print version and exit",
        prog, prog
    );
}
