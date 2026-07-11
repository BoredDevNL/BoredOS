# BoredOS Toolchain

BoredOS is cross-compiled from a host operating system (Linux, macOS, or FreeBSD) using a dedicated, branded `x86_64-boredos` cross-compilation toolchain.

---

## What is the `x86_64-boredos` Toolchain?

The `x86_64-boredos` toolchain is a freestanding cross-compiler environment (based on Binutils 2.42 and GCC 14.2.0). 

### How it Differs from standard `x86_64-elf`
1. **Name Branding**: Instead of using generic `x86_64-elf-*` names, it exposes compiler and binutils binaries under the `x86_64-boredos-*` triple (e.g., `x86_64-boredos-gcc`, `x86_64-boredos-ld`).
2. **Dodging Upstream Patches**: Standard custom OS targets typically require patching GCC's internals (`config.sub`, spec files, etc.) which is tedious to maintain and compiles very slowly in CI. The BoredOS toolchain builds using the universally supported `x86_64-elf` target under the hood, and then exposes clean relative symlinks to provide the `x86_64-boredos-*` tools.
3. **Freestanding and Isolated**: Just like `x86_64-elf`, it is built `--without-headers`. This isolates it entirely from your host operating system's standard libraries and system headers (e.g. GNU glibc or macOS SDKs), preventing host header pollution. Target-specific headers are explicitly supplied by `mlibc` during compilation.
4. **Prebuilt Relocatable Release**: The toolchain is compiled on GitHub Actions for multiple platforms (Linux, macOS Apple Silicon, and FreeBSD). The binaries are fully relocatable and can be installed in any folder.

---

## Installation

You can install the toolchain on almost any Unix-like host machine in a few seconds using the stream-based installer.

### Supported Platforms
* **Linux (x86_64)**
* **macOS (Apple Silicon / arm64)**
* **FreeBSD (x86_64)**

### Standard Install
Run the installation script from the root of the `BoredOS` repository. By default, it installs to `$HOME/boredos-toolchain`:

```bash
bash toolchain/install.sh ~/boredos-toolchain
```

*Note: The installer directly streams the compressed compiler archive over the network to `tar` to avoid downloading large temporary files to your disk.*

---

## Makefile Integration (Zero Config)

You do **not** need to manually add the toolchain to your shell's global `PATH` environment variable. 

The BoredOS root [Makefile](file:///Users/chris/BoredOS/Makefile) automatically detects the toolchain in standard locations:
1. Already present in your shell's `PATH`
2. Installed in `/opt/boredos-toolchain/bin`
3. Installed in `$HOME/boredos-toolchain/bin` (default)

If detected, the root `Makefile` automatically prepends the toolchain directory to the `PATH` environment variable and exports it to all userland submodules and Meson builds. You can simply run:

```bash
make
```

---

## Manual Path Configuration (Optional)

If you installed the toolchain in a custom path not listed above, you can manually export it before running `make`:

```bash
export PATH="/path/to/custom/boredos-toolchain/bin:$PATH"
make
```
