# Build Toolchain

BoredOS is cross-compiled from a host operating system (Linux, macOS, or Windows) and targets the generic `x86_64-elf` platform.

---

# Table of Contents

- [Prerequisites](#prerequisites)
- [Linux (Recommended)](#linux-recommended)
- [Linux (Manual Build)](#linux-manual-build)
- [macOS](#macos)
- [Windows (MSYS2)](#windows-msys2)

---

# Prerequisites

BoredOS requires the following tools.

## x86_64 ELF Cross Toolchain

The BoredOS build system uses a freestanding x86_64 ELF cross-compilation toolchain.

Required components:

- `x86_64-elf-gcc` — Cross C compiler
- `x86_64-elf-g++` — Cross C++ compiler
- `x86_64-elf-ld` — Cross linker
- `x86_64-elf-ar` — Static library archiver
- `x86_64-elf-ranlib` — Archive index generator
- `x86_64-elf-objcopy` — Binary conversion utility
- `x86_64-elf-strip` — Symbol stripping utility

These tools target the generic `x86_64-elf` platform and are completely independent from the host operating system.

## NASM

Used to assemble architecture-specific code.

## xorriso

Used to create bootable ISO images.

## QEMU (Optional)

Recommended for testing and development.

---

# Linux (Recommended)

The recommended way to install the BoredOS toolchain on Linux is to use the provided installer script.

The installer automatically:

- Detects the Linux distribution
- Checks installed tools
- Installs required dependencies
- Downloads Binutils
- Downloads GCC
- Builds Binutils
- Builds GCC
- Installs the toolchain
- Configures the PATH

Run:

```bash
chmod +x tools/install_deps_linux.sh
./tools/install_deps_linux.sh
```

The installer checks the availability of the following tools before starting.

### Cross Toolchain

```text
x86_64-elf-gcc
x86_64-elf-g++
x86_64-elf-ld
```

### Required Build Tools

```text
gcc
g++
make
nasm
xorriso
git
wget
tar
xz
patch
meson
ninja
```

### Optional Tools

```text
qemu-system-x86_64
```

The toolchain is installed by default into:

```text
~/.local/cross
```

The installer automatically appends the following entry to your shell configuration:

```bash
export PATH="$HOME/.local/cross/bin:$PATH"
```

---

# Linux (Manual Build)

If you prefer to build the toolchain manually:

## Download Sources

```bash
wget https://ftp.gnu.org/gnu/binutils/binutils-2.45.tar.xz
wget https://ftp.gnu.org/gnu/gcc/gcc-16.1.0/gcc-16.1.0.tar.xz
```

## Build Binutils

```bash
mkdir build-binutils
cd build-binutils

../binutils-2.45/configure \
    --target=x86_64-elf \
    --prefix=$HOME/.local/cross \
    --with-sysroot \
    --disable-nls \
    --disable-werror

make -j$(nproc)
make install

cd ..
```

## Build GCC

```bash
mkdir build-gcc
cd build-gcc

../gcc-16.1.0/configure \
    --target=x86_64-elf \
    --prefix=$HOME/.local/cross \
    --disable-nls \
    --enable-languages=c,c++ \
    --without-headers

make -j$(nproc) all-gcc
make -j$(nproc) all-target-libgcc

make install-gcc
make install-target-libgcc

cd ..
```

## Add to PATH

```bash
export PATH="$HOME/.local/cross/bin:$PATH"
```

## Verify Installation

```bash
x86_64-elf-gcc --version
x86_64-elf-g++ --version
x86_64-elf-ld --version
```

---

# macOS

## Install Xcode Command Line Tools

```bash
xcode-select --install
```

## Install Homebrew

If Homebrew is not already installed:

```bash
/bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"
```

## Install Required Dependencies

```bash
brew install \
    gcc \
    gmp \
    mpfr \
    libmpc \
    texinfo \
    wget \
    xz \
    nasm \
    xorriso \
    gnu-tar \
    git \
    meson \
    ninja
```

## Build the Toolchain

Follow the same [Manual Build instructions](#linux-manual-build) described in the Linux section.

The resulting toolchain should be installed into:

```text
~/.local/cross
```

## Verify Installation

```bash
x86_64-elf-gcc --version
x86_64-elf-g++ --version
x86_64-elf-ld --version
```

---

# Windows (MSYS2)

## Install MSYS2

Download and install:

https://www.msys2.org/

Launch the **MSYS2 UCRT64** terminal.

---

## Update MSYS2

```bash
pacman -Syu
```

Restart the terminal if requested and repeat until no updates remain.

---

## Install Required Packages

```bash
pacman -S \
    make \
    nasm \
    xorriso \
    git \
    wget \
    tar \
    lz4 \
    xz \
    meson \
    ninja
```

---

## Install QEMU (Optional)

Download:

https://qemu.weilnetz.de/w64/

Add the QEMU installation directory to your PATH.

If image creation fails, you may also need:

```bash
export PATH="/c/Program Files/qemu:$PATH"
```

---

## Install the x86_64 ELF Toolchain

### Recommended

Download a prebuilt x86_64-elf toolchain:

https://github.com/lordmilko/i686-elf-tools/releases

Extract it somewhere convenient.

Add the toolchain to your PATH:

```bash
export PATH="/c/path/to/x86_64-elf-tools/bin:$PATH"
```

To make it permanent:

```bash
echo 'export PATH="/c/path/to/x86_64-elf-tools/bin:$PATH"' >> ~/.bashrc
source ~/.bashrc
```

---

## Verify Installation

```bash
x86_64-elf-gcc --version
x86_64-elf-g++ --version
x86_64-elf-ld --version

nasm -v

qemu-system-x86_64 --version
```

If all commands succeed, the BoredOS development environment is correctly configured and ready for development.
