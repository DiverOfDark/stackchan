use std::path::Path;
use std::process::Command;

fn main() {
    embuild::espidf::sysenv::output();

    // Build and embed the web UI (web/dist/index.html.gz). Set
    // FEMTO_SKIP_WEB=1 to reuse whatever is already built.
    let web = Path::new(env!("CARGO_MANIFEST_DIR")).join("../web");
    println!("cargo:rerun-if-changed={}", web.join("src").display());
    println!("cargo:rerun-if-changed={}", web.join("index.html").display());
    println!("cargo:rerun-if-env-changed=FEMTO_SKIP_WEB");
    let out = web.join("dist/index.html.gz");
    if std::env::var_os("FEMTO_SKIP_WEB").is_some() && out.exists() {
        return;
    }
    if !web.join("node_modules").exists() {
        run(Command::new("npm").arg("ci").current_dir(&web));
    }
    run(Command::new("npm").args(["run", "build"]).current_dir(&web));
    assert!(out.exists(), "web build did not produce {}", out.display());
}

fn run(cmd: &mut Command) {
    let status = cmd.status().unwrap_or_else(|e| panic!("running {cmd:?}: {e} (is Node.js installed?)"));
    assert!(status.success(), "{cmd:?} failed");
}
