use std::fs;
use std::io::{self, BufRead, BufReader, Write};
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;
use std::thread;
use std::time::{Duration, Instant};

use base64::{engine::general_purpose::URL_SAFE_NO_PAD, Engine as _};
use serde_json::Value;

const DEVICE_NAME: &str = "wii-u-setup";
const AUTH_TIMEOUT_SECS: u64 = 120;

// ── Formatting ────────────────────────────────────────────────────────────────

fn sep(c: char) { println!("{}", c.to_string().repeat(60)); }
fn step(n: u8, msg: &str) { sep('─'); println!("  Step {n}: {msg}"); sep('─'); }
fn ok(msg: &str)   { println!("  ✓ {msg}"); }
fn info(msg: &str) { println!("  · {msg}"); }
fn warn(msg: &str) { println!("  ! {msg}"); }

fn wait_for_enter() {
    println!("  Press Enter to close...");
    io::stdout().flush().ok();
    let mut buf = String::new();
    io::stdin().read_line(&mut buf).ok();
}

fn fail(msg: &str) -> ! {
    eprintln!("\n  ERROR: {msg}");
    wait_for_enter();
    std::process::exit(1);
}

fn prompt(msg: &str) -> String {
    print!("{msg}");
    io::stdout().flush().ok();
    let mut buf = String::new();
    io::stdin().read_line(&mut buf).ok();
    buf.trim().to_string()
}

// ── Auth type helpers ─────────────────────────────────────────────────────────

fn auth_type_name(t: u8) -> &'static str {
    match t {
        0 => "AUTHENTICATION_USER_PASS",
        1 => "AUTHENTICATION_STORED_SPOTIFY_CREDENTIALS",
        2 => "AUTHENTICATION_STORED_FACEBOOK_CREDENTIALS",
        3 => "AUTHENTICATION_SPOTIFY_TOKEN",
        4 => "AUTHENTICATION_FACEBOOK_TOKEN",
        _ => "UNKNOWN",
    }
}

fn auth_type_from_str(s: &str) -> u8 {
    match s {
        "AUTHENTICATION_USER_PASS"                    => 0,
        "AUTHENTICATION_STORED_SPOTIFY_CREDENTIALS"   => 1,
        "AUTHENTICATION_STORED_FACEBOOK_CREDENTIALS"  => 2,
        "AUTHENTICATION_SPOTIFY_TOKEN"                => 3,
        "AUTHENTICATION_FACEBOOK_TOKEN"               => 4,
        _                                             => 1,
    }
}

// ── librespot location ────────────────────────────────────────────────────────

fn find_librespot() -> Option<PathBuf> {
    let exe = if cfg!(windows) { "librespot.exe" } else { "librespot" };

    // Same directory as this binary
    if let Ok(mut self_dir) = std::env::current_exe() {
        self_dir.pop();
        let p = self_dir.join(exe);
        if p.exists() { return Some(p); }
    }

    // PATH
    let sep = if cfg!(windows) { ';' } else { ':' };
    if let Ok(path_var) = std::env::var("PATH") {
        for dir in path_var.split(sep) {
            let p = PathBuf::from(dir).join(exe);
            if p.exists() { return Some(p); }
        }
    }

    None
}

// ── Credential conversion ─────────────────────────────────────────────────────

fn convert(creds_json: &Path, out: &Path) -> Result<(), Box<dyn std::error::Error>> {
    let data: Value = serde_json::from_str(&fs::read_to_string(creds_json)?)?;

    let username = data["username"].as_str().unwrap_or("<unknown>");

    let (auth_type, b64): (u8, &str) = if let Some(d) = data["auth_data"].as_str() {
        (data["auth_type"].as_u64().unwrap_or(1) as u8, d)
    } else if let Some(d) = data["credentials"].as_str() {
        let t = data["type"].as_str()
            .unwrap_or("AUTHENTICATION_STORED_SPOTIFY_CREDENTIALS");
        (auth_type_from_str(t), d)
    } else {
        return Err("no 'auth_data' or 'credentials' field in credentials.json".into());
    };

    let blob = URL_SAFE_NO_PAD.decode(b64.trim_end_matches('='))?;

    let mut payload = vec![auth_type];
    payload.extend_from_slice(&blob);
    fs::write(out, &payload)?;

    ok(&format!("username  : {username}"));
    ok(&format!("auth_type : {auth_type} ({})", auth_type_name(auth_type)));
    ok(&format!("blob size : {} bytes", blob.len()));
    Ok(())
}

// ── SD card detection ─────────────────────────────────────────────────────────

#[cfg(target_os = "windows")]
fn find_sd_card() -> Option<PathBuf> {
    let mut fallback = None;
    for letter in b'A'..=b'Z' {
        if letter == b'C' { continue; }
        let root = PathBuf::from(format!("{}:\\", letter as char));
        if !root.exists() { continue; }
        if root.join("wiiu").exists() || root.join("WIIU").exists() {
            return Some(root);
        }
        if fallback.is_none() { fallback = Some(root); }
    }
    fallback
}

#[cfg(target_os = "macos")]
fn find_sd_card() -> Option<PathBuf> {
    let mut fallback = None;
    for entry in fs::read_dir("/Volumes").ok()?.flatten() {
        let p = entry.path();
        if !p.is_dir() { continue; }
        if p.join("wiiu").exists() || p.join("WIIU").exists() { return Some(p); }
        let name = p.file_name().and_then(|n| n.to_str()).unwrap_or("");
        if name != "Macintosh HD" && fallback.is_none() { fallback = Some(p); }
    }
    fallback
}

#[cfg(not(any(target_os = "windows", target_os = "macos")))]
fn find_sd_card() -> Option<PathBuf> {
    let user = std::env::var("USER").unwrap_or_default();
    let bases = [
        format!("/media/{user}"),
        "/media".to_string(),
        "/mnt".to_string(),
        format!("/run/media/{user}"),
    ];
    let candidates: Vec<PathBuf> = bases
        .iter()
        .filter_map(|b| fs::read_dir(b).ok())
        .flatten()
        .flatten()
        .map(|e| e.path())
        .filter(|p| p.is_dir())
        .collect();

    // Prefer a volume that already has wiiu/
    if let Some(p) = candidates.iter().find(|p| {
        p.join("wiiu").exists() || p.join("WIIU").exists()
    }) {
        return Some(p.clone());
    }
    candidates.into_iter().next()
}

// ── Subprocess reader thread ──────────────────────────────────────────────────

fn spawn_log_reader(
    stream: impl io::Read + Send + 'static,
    flag: Arc<AtomicBool>,
) {
    thread::spawn(move || {
        for line in BufReader::new(stream).lines().flatten() {
            println!("  [librespot] {line}");
            if line.to_lowercase().contains("authenticated") {
                flag.store(true, Ordering::Relaxed);
            }
        }
    });
}

// ── Main ──────────────────────────────────────────────────────────────────────

fn main() {
    println!();
    println!("  ╔══════════════════════════════════════════════╗");
    println!("  ║        Spotify Wii U — Setup Tool           ║");
    println!("  ╚══════════════════════════════════════════════╝");
    println!();

    // Step 1 ── find librespot ──────────────────────────────────────────────
    step(1, "Locating librespot");

    let librespot = find_librespot().unwrap_or_else(|| fail(
        "librespot not found.\n\n  \
        Option A: Place the librespot binary alongside this executable and run again.\n\n  \
        Option B: Install librespot:\n    \
          Windows/Linux/macOS:  cargo install librespot\n    \
          Linux (apt):          sudo apt install librespot\n    \
          Linux (pacman):       sudo pacman -S librespot\n\n  \
        Prebuilt binaries are attached to the librespot-tools release on GitHub."
    ));

    ok(&format!("Found: {}", librespot.display()));

    // Step 2 ── run librespot, wait for Spotify auth ───────────────────────
    step(2, "Authenticating with Spotify");

    let cache_dir  = std::env::temp_dir().join("librespot-cache-wiiu");
    let creds_file = cache_dir.join("credentials.json");

    let _ = fs::remove_file(&creds_file);   // clear stale credentials
    fs::create_dir_all(&cache_dir).unwrap_or_else(|e| fail(&format!("Cache dir: {e}")));

    info(&format!("Cache: {}", cache_dir.display()));
    println!();
    println!("  ┌──────────────────────────────────────────────────────┐");
    println!("  │  Open the Spotify app, go to the device list, and  │");
    println!("  │  select:  wii-u-setup                               │");
    println!("  │                                                      │");
    println!("  │  Waiting up to {AUTH_TIMEOUT_SECS}s for authentication...         │");
    println!("  └──────────────────────────────────────────────────────┘");
    println!();

    let mut child = Command::new(&librespot)
        .args(["--name", DEVICE_NAME, "--cache"])
        .arg(&cache_dir)
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .spawn()
        .unwrap_or_else(|e| {
            if e.kind() == io::ErrorKind::PermissionDenied {
                fail(&format!(
                    "Cannot execute {} — permission denied.\n  \
                    Run:  chmod +x {}",
                    librespot.display(), librespot.display()
                ));
            }
            fail(&format!("Failed to start librespot: {e}"));
        });

    let flag = Arc::new(AtomicBool::new(false));
    if let Some(s) = child.stdout.take() { spawn_log_reader(s, Arc::clone(&flag)); }
    if let Some(s) = child.stderr.take() { spawn_log_reader(s, Arc::clone(&flag)); }

    let deadline = Instant::now() + Duration::from_secs(AUTH_TIMEOUT_SECS);
    while Instant::now() < deadline {
        if flag.load(Ordering::Relaxed) || creds_file.exists() {
            thread::sleep(Duration::from_millis(500));
            break;
        }
        thread::sleep(Duration::from_millis(500));
    }

    let _ = child.kill();
    let _ = child.wait();
    println!();
    ok("librespot stopped");

    if !flag.load(Ordering::Relaxed) && !creds_file.exists() {
        fail(&format!(
            "Timed out after {AUTH_TIMEOUT_SECS}s without authentication.\n  \
            Select 'wii-u-setup' in the Spotify device list and try again."
        ));
    }

    if !creds_file.exists() {
        fail("credentials.json was not written — try running again.");
    }

    // Step 3 ── convert ────────────────────────────────────────────────────
    step(3, "Converting credentials");

    let out = PathBuf::from("spotify_saved_creds.bin");
    convert(&creds_file, &out).unwrap_or_else(|e| fail(&format!("Conversion failed: {e}")));

    let out_abs = out.canonicalize().unwrap_or_else(|_| out.clone());
    ok(&format!("Written: {}", out_abs.display()));

    // Step 4 ── copy to SD card ────────────────────────────────────────────
    step(4, "Copying to SD card");

    match find_sd_card() {
        Some(sd) => {
            info(&format!("Detected: {}", sd.display()));
            let answer = prompt("  Copy spotify_saved_creds.bin to SD card? [Y/n]: ");
            if answer.is_empty() || answer.eq_ignore_ascii_case("y") {
                let dest = sd.join("spotify_saved_creds.bin");
                fs::copy(&out, &dest).unwrap_or_else(|e| fail(&format!("Copy failed: {e}")));
                ok(&format!("Copied to {}", dest.display()));
            } else {
                info("Skipped.");
            }
        }
        None => {
            warn("No SD card detected.");
            info(&format!(
                "Copy manually:  {}  →  SD:/spotify_saved_creds.bin",
                out_abs.display()
            ));
        }
    }

    // Done ─────────────────────────────────────────────────────────────────
    sep('═');
    println!();
    println!("  Setup complete!");
    println!();
    println!("  1. Insert the SD card in your Wii U");
    println!("  2. Open the Aroma Homebrew Launcher");
    println!("  3. Launch Spotify Wii U");
    println!("  4. Select 'Wii U' from any Spotify app's device list");
    println!();
    sep('═');
    println!();
    wait_for_enter();
}
