// Compiles the C core (../core) into the app, so the binary is standalone.
use std::{env, path::PathBuf, process::Command};

fn main() {
    let core = PathBuf::from("../core");
    for f in ["memslammer.c", "engine.c", "memslammer.h", "internal.h"] {
        println!("cargo:rerun-if-changed=../core/{f}");
    }
    println!("cargo:rerun-if-env-changed=MS_VERSION");
    println!("cargo:rerun-if-changed=../res/gui.rc");
    println!("cargo:rerun-if-changed=../res/common.rc");

    let version = env::var("MS_VERSION").ok().filter(|v| !v.is_empty()).unwrap_or_else(|| {
        Command::new("git")
            .args(["describe", "--tags", "--always", "--dirty"])
            .output()
            .ok()
            .and_then(|o| String::from_utf8(o.stdout).ok())
            .map(|s| s.trim().trim_start_matches('v').to_string())
            .filter(|s| !s.is_empty())
            .unwrap_or_else(|| "dev".into())
    });
    let msvc = env::var("CARGO_CFG_TARGET_ENV").as_deref() == Ok("msvc");

    let mut base = cc::Build::new();
    base.define("MS_VERSION", format!("\"{version}\"").as_str()).opt_level(3).warnings(false);
    if msvc {
        base.flag("/std:c17");
    } else {
        base.flag("-std=gnu11");
    }
    // Order matters for static linking: the control part references the engine.
    base.clone().file(core.join("memslammer.c")).compile("ms_core");
    let mut engine = base.clone();
    engine.file(core.join("engine.c")).flag(if msvc { "/arch:AVX2" } else { "-mavx2" });
    engine.compile("ms_engine");

    println!("cargo:rustc-env=MS_VERSION={version}");

    if env::var("CARGO_CFG_TARGET_OS").as_deref() == Ok("windows") {
        // icon + version info (gpui embeds its own manifest)
        let out = PathBuf::from(env::var("OUT_DIR").unwrap());
        let nums: Vec<u32> = version
            .split(|c: char| !c.is_ascii_digit())
            .take(3)
            .map(|n| n.parse().unwrap_or(0))
            .collect();
        let comma = if version.chars().next().is_some_and(|c| c.is_ascii_digit()) && nums.len() == 3 {
            format!("{},{},{},0", nums[0], nums[1], nums[2])
        } else {
            "0,0,0,0".into()
        };
        std::fs::write(
            out.join("version.h"),
            format!("#define MS_VER_COMMA {comma}\n#define MS_VERSION_STR \"{version}\"\n"),
        )
        .unwrap();
        embed_resource::compile("../res/gui.rc", embed_resource::ParamsIncludeDirs([out, PathBuf::from("../res")]))
            .manifest_optional()
            .unwrap();
    }
}
