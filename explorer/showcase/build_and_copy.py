#!/usr/bin/env python3
# Copyright 2025 The Lynx Authors. All rights reserved.
# Licensed under the Apache License Version 2.0 that can be found in the
# LICENSE file in the root directory of this source tree.
import os
import shutil
import subprocess
import sys

# Get the directory where the current script is located
current_dir = os.path.dirname(os.path.realpath(__file__))
# Get the root directory
root_dir = os.path.abspath(os.path.join(current_dir, '../../'))
sys.path.append(root_dir)

# Define the Lynx example directory name
LYNX_EXAMPLE_DIR_NAME = "@lynx-example"
YEW_WASM_WORKSPACE_DIR = os.path.join(root_dir, "rust", "yew")
RUST_WASM_TARGET_DIR = os.path.join(root_dir, "out", "rust_wamr_wasm")
RUST_WASM_SHOWCASE_DIR = "rust-wasm"
RUST_WASM_EXAMPLES = [
    {
        "dir": os.path.join(root_dir, "rust", "yew", "examples", "react"),
        "package": "react-lynx-yew",
        "wasm_file": "react.wasm",
    },
    {
        "dir": os.path.join(root_dir, "rust", "yew", "examples", "colorful-view"),
        "package": "colorful-view-yew",
        "wasm_file": "colorful-view.wasm",
    },
    {
        "dir": os.path.join(root_dir, "rust", "dioxus", "examples", "lynx-react"),
        "package": "lynx-react",
        "wasm_file": "dioxus-react.wasm",
    },
    {
        "dir": os.path.join(root_dir, "rust", "dioxus", "examples", "lynx-colorful-view"),
        "package": "lynx-colorful-view",
        "wasm_file": "dioxus-colorful-view.wasm",
    },
]
RUST_WASM_TARGET_FEATURES = ",".join([
    "+bulk-memory",
    "+bulk-memory-opt",
    "+multivalue",
    "+mutable-globals",
    "+nontrapping-fptoint",
    "+reference-types",
    "+sign-ext",
    "+tail-call",
])
RUST_WASM_RELEASE_PROFILE = {
    "CARGO_PROFILE_RELEASE_OPT_LEVEL": "3",
    "CARGO_PROFILE_RELEASE_LTO": "true",
    "CARGO_PROFILE_RELEASE_CODEGEN_UNITS": "1",
    "CARGO_PROFILE_RELEASE_PANIC": "abort",
    "CARGO_PROFILE_RELEASE_INCREMENTAL": "false",
}
WASM_OPT_FLAGS = [
    "-O3",
    "--strip-debug",
    "--strip-dwarf",
    "--strip-producers",
    "--enable-sign-ext",
    "--enable-mutable-globals",
    "--enable-nontrapping-float-to-int",
    "--enable-bulk-memory",
    "--enable-bulk-memory-opt",
    "--enable-reference-types",
    "--enable-multivalue",
    "--enable-tail-call",
    "--enable-gc",
    "--enable-memory64",
    "--enable-multimemory",
]


def run_checked_command(command, cwd, env=None):
    print(f"Run command: {' '.join(command)}")
    subprocess.run(command, cwd=cwd, env=env, check=True)


def ensure_rust_wasm_workspace_manifest():
    generated_manifests = []
    for manifest_name in ["Cargo.toml", "Cargo.lock"]:
        manifest_path = os.path.join(YEW_WASM_WORKSPACE_DIR, manifest_name)
        backup_path = os.path.join(YEW_WASM_WORKSPACE_DIR,
                                   f"{manifest_name}.backup")
        if os.path.exists(manifest_path):
            continue
        if not os.path.exists(backup_path):
            continue
        shutil.copy(backup_path, manifest_path)
        generated_manifests.append(manifest_path)
    return generated_manifests


def remove_generated_workspace_manifests(manifests):
    for manifest_path in manifests:
        if os.path.exists(manifest_path):
            os.remove(manifest_path)


def remove_generated_file(path, existed_before):
    if not existed_before and os.path.exists(path):
        os.remove(path)


def build_rust_wasm_file(example):
    print("========== build Rust WAMR showcase ==========")
    example_dir = example["dir"]
    if not os.path.exists(example_dir):
        raise FileNotFoundError(
            f"Rust wasm example directory not found: {example_dir}")

    env = os.environ.copy()
    rustflags = f"-C target-feature={RUST_WASM_TARGET_FEATURES}"
    target_rustflags = "CARGO_TARGET_WASM32_WASIP1_RUSTFLAGS"
    if env.get(target_rustflags):
        rustflags = f"{env[target_rustflags]} {rustflags}"
    env[target_rustflags] = rustflags
    env.update(RUST_WASM_RELEASE_PROFILE)
    env["CARGO_TARGET_DIR"] = RUST_WASM_TARGET_DIR

    example_lock_path = os.path.join(example_dir, "Cargo.lock")
    example_lock_existed = os.path.exists(example_lock_path)
    generated_manifests = ensure_rust_wasm_workspace_manifest()
    try:
        run_checked_command([
            "cargo",
            "build",
            "--manifest-path",
            os.path.join(example_dir, "Cargo.toml"),
            "--target",
            "wasm32-wasip1",
            "--release",
        ], example_dir, env)
    finally:
        remove_generated_workspace_manifests(generated_manifests)
        remove_generated_file(example_lock_path, example_lock_existed)

    release_dir = os.path.join(RUST_WASM_TARGET_DIR, "wasm32-wasip1",
                               "release")
    package_name = example["package"]
    wasm_candidates = [
        os.path.join(release_dir, f"{package_name}.wasm"),
        os.path.join(release_dir, f"{package_name.replace('-', '_')}.wasm"),
    ]
    for candidate in wasm_candidates:
        if os.path.exists(candidate):
            source_wasm = candidate
            break
    else:
        raise FileNotFoundError(
            f"Unable to find Rust wasm output in {release_dir}")

    optimized_wasm = os.path.join(release_dir, example["wasm_file"])
    run_checked_command(
        ["wasm-opt", *WASM_OPT_FLAGS, source_wasm, "-o", optimized_wasm],
        example_dir)
    return optimized_wasm


def copy_rust_wasm_files(wasm_files, showcase_dirs):
    print("========== copy Rust WAMR showcase ==========")
    for showcase_dir in showcase_dirs:
        rust_wasm_dir = os.path.join(showcase_dir, RUST_WASM_SHOWCASE_DIR)
        os.makedirs(rust_wasm_dir, exist_ok=True)
        for wasm_file in wasm_files:
            shutil.copy(wasm_file,
                        os.path.join(rust_wasm_dir, os.path.basename(wasm_file)))


def resolve_pnpm_runner():
    try:
        from tools.js_tools.pnpm_helper import run_pnpm_command
        return run_pnpm_command
    except (ImportError, TypeError):
        pnpm_path = shutil.which("pnpm")
        if pnpm_path is None:
            raise

        def run_system_pnpm_command(command, cwd, env=None):
            resolved_command = list(command)
            if resolved_command and resolved_command[0] == "pnpm":
                resolved_command[0] = pnpm_path
            run_checked_command(resolved_command, cwd, env)

        return run_system_pnpm_command


def main():
    run_pnpm_command = resolve_pnpm_runner()
    # Get the showcase root directory
    showcase_root_dir = os.path.dirname(os.path.abspath(__file__))
    explorer_dir = os.path.dirname(showcase_root_dir)

    # Define Android and iOS asset directories
    android_assets_dir = os.path.join(explorer_dir, "android", "lynx_explorer",
                                      "src", "main", "assets")
    ios_resource_dir = os.path.join(explorer_dir, "darwin", "ios",
                                    "lynx_explorer", "LynxExplorer",
                                    "Resource")
    harmony_dir = os.path.join(explorer_dir, "harmony", "lynx_explorer", "src",
                               "main", "resources", "rawfile")
    # Define Windows resource directory
    windows_resource_dir = os.path.join(explorer_dir, "windows",
                                        "lynx_explorer", "resources")
    macos_resource_dir = os.path.join(explorer_dir, "darwin", "macos",
                                      "lynx_explorer", "Resource")

    print(f"macOS resource directory: {macos_resource_dir}")
    print(f"macOS resource directory exists: {os.path.exists(macos_resource_dir)}")

    # Create Android and iOS asset directories if they don't exist
    if not os.path.exists(android_assets_dir):
        os.makedirs(android_assets_dir)
    if not os.path.exists(ios_resource_dir):
        os.makedirs(ios_resource_dir)
    if not os.path.exists(harmony_dir):
        os.makedirs(harmony_dir)
    # Create Windows/macos resource directory if it doesn't exist
    if not os.path.exists(windows_resource_dir):
        os.makedirs(windows_resource_dir)
    if not os.path.exists(macos_resource_dir):
        os.makedirs(macos_resource_dir)

    # Remove existing showcase directories and create new ones
    showcase_android = os.path.join(android_assets_dir, "showcase")
    showcase_ios = os.path.join(ios_resource_dir, "showcase")
    showcase_harmony = os.path.join(harmony_dir, "showcase")
    # Define Windows/macos showcase directory
    showcase_windows = os.path.join(windows_resource_dir, "showcase")
    showcase_macos = os.path.join(macos_resource_dir, "showcase")

    if os.path.exists(showcase_android):
        shutil.rmtree(showcase_android)
    if os.path.exists(showcase_ios):
        shutil.rmtree(showcase_ios)
    if os.path.exists(showcase_harmony):
        shutil.rmtree(showcase_harmony)
    if os.path.exists(showcase_windows):
        shutil.rmtree(showcase_windows)
    if os.path.exists(showcase_macos):
        shutil.rmtree(showcase_macos)

    os.makedirs(showcase_android)
    os.makedirs(showcase_ios)
    os.makedirs(showcase_harmony)
    os.makedirs(showcase_windows)
    os.makedirs(showcase_macos)

    showcase_dirs = [
        showcase_android,
        showcase_ios,
        showcase_harmony,
        showcase_windows,
        showcase_macos,
    ]

    print("========== build showcase page ==========")
    os.chdir(showcase_root_dir)
    # Install dependencies and build
    run_pnpm_command(["pnpm", "install", "--frozen-lockfile"], os.getcwd())
    run_pnpm_command(["pnpm", "run", "build"], os.getcwd())
    rust_wasm_files = [
        build_rust_wasm_file(example) for example in RUST_WASM_EXAMPLES
    ]

    print("========== copy showcase resource ==========")
    # Copy resources from node_modules
    node_modules_example_dir = os.path.join(showcase_root_dir, "node_modules",
                                            LYNX_EXAMPLE_DIR_NAME)
    for path in os.listdir(node_modules_example_dir):
        path_android = os.path.join(showcase_android, path)
        path_ios = os.path.join(showcase_ios, path)
        path_harmony = os.path.join(showcase_harmony, path)
        path_windows = os.path.join(showcase_windows, path)
        path_macos = os.path.join(showcase_macos, path)
        os.makedirs(path_android)
        os.makedirs(path_ios)
        os.makedirs(path_harmony)
        os.makedirs(path_windows)
        os.makedirs(path_macos)

        dist_dir = os.path.join(node_modules_example_dir, path, "dist")
        for filename in os.listdir(dist_dir):
            if filename.endswith(".lynx.bundle"):
                shutil.copy(os.path.join(dist_dir, filename), path_android)
                shutil.copy(os.path.join(dist_dir, filename), path_ios)
                shutil.copy(os.path.join(dist_dir, filename), path_harmony)
                shutil.copy(os.path.join(dist_dir, filename), path_windows)
                shutil.copy(os.path.join(dist_dir, filename), path_macos)

    # Copy menu resources
    menu_dist_dir = os.path.join(showcase_root_dir, "menu", "dist")

    menu_android = os.path.join(showcase_android, "menu")
    menu_ios = os.path.join(showcase_ios, "menu")
    menu_harmony = os.path.join(showcase_harmony, "menu")
    menu_windows = os.path.join(showcase_windows, "menu")
    menu_macos = os.path.join(showcase_macos, "menu")

    print("Creating menu directories")
    os.makedirs(menu_android)
    os.makedirs(menu_ios)
    os.makedirs(menu_harmony)
    os.makedirs(menu_windows)
    os.makedirs(menu_macos)
    for filename in os.listdir(menu_dist_dir):
        if filename.endswith(".lynx.bundle"):
            shutil.copy(os.path.join(menu_dist_dir, filename), menu_android)
            shutil.copy(os.path.join(menu_dist_dir, filename), menu_ios)
            shutil.copy(os.path.join(menu_dist_dir, filename), menu_harmony)
            shutil.copy(os.path.join(menu_dist_dir, filename), menu_windows)
            shutil.copy(os.path.join(menu_dist_dir, filename), menu_macos)

    copy_rust_wasm_files(rust_wasm_files, showcase_dirs)


if __name__ == "__main__":
    main()
