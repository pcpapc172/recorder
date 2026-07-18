# AI Agent Build Instructions — Ubuntu on WSL

This file is for an AI coding agent building this repository from an Ubuntu WSL shell.

## Evidence policy

Do not invent commands, package names, paths, Geode APIs, CMake flags, or fixes.

The build procedure below is derived from these files in the supplied Geode documentation archive:

- `getting-started/cpp-stuff.md`, section **Linux**
- `getting-started/geode-cli.md`, sections **Linux** and **Profile Setup**
- `getting-started/sdk.md`, sections **Setting up the SDK** and **Cache**
- `getting-started/create-mod.md`, sections **Build** and **Building Windows mods on Linux**

The documentation does not mention WSL by name. The only inference in this file is that an Ubuntu distribution running under WSL uses the documented Ubuntu/Linux cross-compilation procedure. Do not infer anything further without checking the supplied docs or an official upstream source.

## Target

Geometry Dash has no official Linux release according to the supplied Geode docs. A build performed in Ubuntu WSL therefore cross-compiles the Windows version of this Geode mod.

Repository metadata currently declares:

- Geode: `5.4.1`
- Geometry Dash for Windows: `2.2081`
- C++ standard: C++20
- Required mod dependency: `eclipse.ffmpeg-api >= 2.0.1`

Do not silently change those versions.

## 1. Work from the repository root

The repository root is the directory containing all of these files:

```text
CMakeLists.txt
mod.json
src/
aiagent.md
```

Before running a build, verify the current directory:

```bash
pwd
ls
```

Stop if `CMakeLists.txt` or `mod.json` is absent.

## 2. Verify required base tools

The supplied Geode docs require:

- Git
- CMake 3.29 or newer
- Clang and LLD on Linux

Check first instead of installing blindly:

```bash
git --version
cmake --version
clang-19 --version
lld-19 --version
```

The first line of `cmake --version` must report version 3.29 or newer.

For Ubuntu, the supplied docs give this exact compiler/toolchain installation command:

```bash
sudo apt install clang-19 clang-tools-19 lld-19
```

The docs state that Git and CMake are also required, but the supplied Ubuntu section does not provide an `apt` command for installing them. Do not fabricate one in an automated repair. If either is missing, report the missing prerequisite and use an official installation method only after it has been verified.

## 3. Install and verify the Geode CLI

The supplied Geode docs say Linux prebuilt binaries are provided on the Geode CLI releases page and that the binary must be placed on the global `PATH`. They do not provide a universal Linux installation command.

Therefore:

1. Obtain the Linux Geode CLI binary from the official Geode CLI releases.
2. Put it somewhere included in the WSL user's `PATH`.
3. Verify it from a new shell:

```bash
geode --version
```

Do not continue until that command succeeds.

Do not invent a `curl`, `wget`, archive filename, installation directory, or release URL from memory. Resolve those details from the official release currently being installed.

## 4. Install the Linux-to-Windows cross-compilation tools

Run the exact command documented by Geode:

```bash
geode sdk install-linux
```

This installs the Windows SDK and CMake toolchain required to cross-compile Windows Geode mods from Linux.

If this command fails, preserve and report its complete output. Do not replace it with undocumented toolchain downloads or guessed CMake paths.

## 5. Install the Geode SDK and binaries

Run the documented SDK commands:

```bash
geode sdk install
geode sdk install-binaries
```

Restart the shell if the installer says environment changes were made. Then verify the SDK environment variable using the documented non-Windows command:

```bash
echo "$GEODE_SDK"
```

The output must be a non-empty path. Verify the path exists:

```bash
test -d "$GEODE_SDK" && printf 'GEODE_SDK exists: %s\n' "$GEODE_SDK"
```

Stop and report the problem if `GEODE_SDK` is empty or the directory does not exist. Do not guess the SDK location.

## 6. Optional documented dependency cache

The Geode docs highly recommend setting `CPM_SOURCE_CACHE` to a permanent directory to avoid duplicate dependency downloads and permit offline rebuilds after dependencies have been fetched once.

The docs do not prescribe a path. If the user or environment already supplies one, verify it:

```bash
printf 'CPM_SOURCE_CACHE=%s\n' "$CPM_SOURCE_CACHE"
```

Do not select a permanent path on the user's behalf unless explicitly authorized.

## 7. Do not require a Geometry Dash profile for packaging

The Geode CLI profile is needed for automatic post-build installation into a Geometry Dash installation. It is not required merely to produce the `.geode` package.

If automatic installation into a WSL-visible Geometry Dash installation is explicitly requested, the documented setup command is:

```bash
geode config setup
```

Do not guess a Windows Steam, Wine, or Geometry Dash directory. Let the CLI setup process or the user provide it.

## 8. Clean stale configuration when necessary

This repository cross-compiles a Windows mod. A build directory configured by a different operating system, SDK, compiler, or toolchain can be invalid.

Before the first verified WSL build, inspect existing build directories:

```bash
find . -maxdepth 1 -type d -name 'build*' -print
```

Do not delete user files automatically. A build directory may be removed only when it is clearly generated output and a clean reconfiguration is needed.

## 9. Preferred documented build command

From the repository root, build using the command documented specifically for Windows mods on Linux:

```bash
geode build
```

The general build documentation also says that when Clang and Ninja are installed, this form may be required:

```bash
geode build --ninja
```

Use `geode build` first for the Linux cross-compilation flow. Use `geode build --ninja` only when the CLI indicates Ninja is required or the ordinary command fails for that documented reason.

Do not append arbitrary `-D` variables or compiler flags.

## 10. Documented CMake fallback

The supplied docs provide this generic fallback when `geode build` cannot be used:

```bash
cmake -B build
cmake --build build --config RelWithDebInfo
```

However, for a Windows mod built on Linux, the docs state that manually installed cross-compilation components must be supplied using this form:

```bash
geode build -- -DCMAKE_TOOLCHAIN_FILE=/path/to/clang-msvc-sdk/clang-msvc.cmake -DSPLAT_DIR=/path/to/splat
```

Do not substitute guessed values for either placeholder. When `geode sdk install-linux` was used successfully, prefer plain `geode build`, which is the documented automatic path.

## 11. Determine success from the build, not assumptions

A successful build must satisfy all of the following:

1. The build command exits with status `0`.
2. A `.geode` package exists in a generated build directory.
3. The package is non-empty.

Check without assuming a fixed build-directory name:

```bash
find . -type f -name '*.geode' -print
```

For each reported package, verify it is non-empty:

```bash
test -s /exact/path/from/find.geode
```

Report the exact package path returned by `find`.

Do not claim the mod was compiled or tested merely because CMake configuration succeeded.

## 12. Dependency and runtime distinction

`mod.json` declares this runtime dependency:

```json
"eclipse.ffmpeg-api": ">=2.0.1"
```

Do not remove or weaken it to make packaging pass. The generated recorder mod requires a compatible Eclipse FFmpeg API mod to be installed in Geometry Dash at runtime.

Do not claim that successful compilation proves FFmpeg encoding works at runtime. Runtime validation requires launching the matching Windows Geometry Dash and Geode versions with `eclipse.ffmpeg-api` installed.

## 13. Error-handling rules for the agent

When a command fails:

1. Capture the full command and full output.
2. Identify the first actionable error, not only the final cascading error.
3. Search the supplied Geode docs for that exact topic.
4. Use official Geode or tool documentation when the supplied docs are insufficient.
5. Clearly distinguish a documented fix from an inference.
6. Do not alter source code, dependency versions, SDK versions, or CMake settings merely to suppress an error.
7. Do not claim success until the checks in section 11 pass.

## 14. Minimal reproducible build transcript

For a prepared Ubuntu WSL environment where the CLI, SDK, binaries, and Linux cross-toolchain are already installed, the documented build sequence is:

```bash
cd /path/to/gd-recorder
geode --version
cmake --version
echo "$GEODE_SDK"
geode build
find . -type f -name '*.geode' -print
```

`/path/to/gd-recorder` is intentionally a placeholder. Replace it only with the actual repository location discovered in the environment; never guess it.
