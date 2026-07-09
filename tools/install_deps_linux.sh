#!/usr/bin/env bash
set -euo pipefail

TARGET=x86_64-elf
PREFIX="${HOME}/.local/cross"

BINUTILS_VER=2.45
GCC_VER=16.1.0

BUILD_DIR="${HOME}/crosscompiler"

GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

MISSING_TOOLS=()

check_tool() {
    local tool="$1"

    if command -v "$tool" >/dev/null 2>&1; then
        echo -e "${GREEN}[FOUND]${NC} $tool"
    else
        echo -e "${RED}[MISSING]${NC} $tool"
        MISSING_TOOLS+=("$tool")
    fi
}

detect_pkgmgr() {
    if command -v dnf >/dev/null 2>&1; then
        echo dnf
    elif command -v apt >/dev/null 2>&1; then
        echo apt
    elif command -v pacman >/dev/null 2>&1; then
        echo pacman
    elif command -v zypper >/dev/null 2>&1; then
        echo zypper
    elif command -v apk >/dev/null 2>&1; then
        echo apk
    else
        echo unknown
    fi
}

install_deps() {
    case "$(detect_pkgmgr)" in

        dnf)
            echo -e "${BLUE}[INFO]${NC} Installing dependencies with DNF..."
            sudo dnf install -y \
                gcc gcc-c++ make \
                bison flex \
                gmp-devel mpfr-devel libmpc-devel \
                texinfo wget tar xz patch \
                nasm xorriso git
            ;;

        apt)
            echo -e "${BLUE}[INFO]${NC} Installing dependencies with APT..."
            sudo apt update
            sudo apt install -y \
                build-essential \
                bison flex \
                libgmp3-dev \
                libmpfr-dev \
                libmpc-dev \
                texinfo wget tar xz-utils patch \
                nasm xorriso git
            ;;

        pacman)
            echo -e "${BLUE}[INFO]${NC} Installing dependencies with Pacman..."
            sudo pacman -Sy --needed \
                base-devel \
                bison flex \
                gmp mpfr libmpc \
                texinfo wget tar xz patch \
                nasm xorriso git
            ;;

        zypper)
            echo -e "${BLUE}[INFO]${NC} Installing dependencies with Zypper..."
            sudo zypper install -y \
                gcc gcc-c++ make \
                bison flex \
                gmp-devel mpfr-devel libmpc-devel \
                texinfo wget tar xz patch \
                nasm xorriso git
            ;;

        apk)
            echo -e "${BLUE}[INFO]${NC} Installing dependencies with APK..."
            sudo apk add \
                build-base \
                bison flex \
                gmp-dev mpfr-dev libmpc1-dev \
                texinfo wget tar xz patch \
                nasm xorriso git
            ;;

        *)
            echo -e "${RED}[ERROR]${NC} Unsupported distribution."
            exit 1
            ;;
    esac
}

clear

echo "=================================================="
echo "      BoredOS Toolchain Installer"
echo "=================================================="
echo

if [ -f /etc/os-release ]; then
    . /etc/os-release
    DISTRO="${PRETTY_NAME:-$NAME}"
else
    DISTRO="Unknown Linux Distribution"
fi

echo -e "${BLUE}[INFO]${NC} Detected distribution: ${DISTRO}"
echo

echo "=================================================="
echo "Checking Toolchain"
echo "=================================================="
echo

check_tool x86_64-elf-gcc
check_tool x86_64-elf-g++
check_tool x86_64-elf-ld

echo

echo "=================================================="
echo "Checking Required Tools"
echo "=================================================="
echo

check_tool gcc
check_tool g++
check_tool make
check_tool nasm
check_tool xorriso
check_tool git
check_tool wget
check_tool tar
check_tool xz
check_tool patch

echo

echo "=================================================="
echo "Checking Optional Tools"
echo "=================================================="
echo

if command -v qemu-system-x86_64 >/dev/null 2>&1; then
    echo -e "${GREEN}[FOUND]${NC} qemu-system-x86_64"
else
    echo -e "${YELLOW}[OPTIONAL]${NC} qemu-system-x86_64"
fi

echo

echo -e "${YELLOW}"
echo "=================================================="
echo "WARNING"
echo "=================================================="
echo
echo "This script will build a complete x86_64-elf"
echo "cross-compilation toolchain from source."
echo
echo -e "Building GCC may take ${RED}between 10 and 60+ minutes ${YELLOW} depending on your system"
echo 
echo "Several gigabytes of disk space may be required"
echo
echo "A stable Internet connection is required."
echo
echo "Toolchain installation path:"
echo
echo "    ${PREFIX}"
echo
echo "=================================================="
echo -e "${NC}"
echo

read -rp "Do you want to continue? [y/N] " REPLY

case "$REPLY" in
    y|Y|yes|YES)
        ;;
    *)
        echo
        echo "Installation cancelled."
        exit 0
        ;;
esac

echo
echo -e "${BLUE}[INFO]${NC} Installing dependencies..."
install_deps

echo
echo -e "${BLUE}[INFO]${NC} Creating build directory..."
mkdir -p "${BUILD_DIR}"
cd "${BUILD_DIR}"

echo
echo -e "${BLUE}[INFO]${NC} Downloading Binutils ${BINUTILS_VER}..."
wget -nc "https://ftp.gnu.org/gnu/binutils/binutils-${BINUTILS_VER}.tar.xz"

echo
echo -e "${BLUE}[INFO]${NC} Downloading GCC ${GCC_VER}..."
wget -nc "https://ftp.gnu.org/gnu/gcc/gcc-${GCC_VER}/gcc-${GCC_VER}.tar.xz"

echo
echo -e "${BLUE}[INFO]${NC} Extracting archives..."
if [ ! -d "binutils-${BINUTILS_VER}" ]; then
    tar -xf "binutils-${BINUTILS_VER}.tar.xz"
fi

if [ ! -d "gcc-${GCC_VER}" ]; then
    tar -xf "gcc-${GCC_VER}.tar.xz"
fi
echo
echo -e "${BLUE}[INFO]${NC} Downloading GCC prerequisites..."
cd "gcc-${GCC_VER}"
./contrib/download_prerequisites
cd ..

echo
echo -e "${BLUE}[INFO]${NC} Building Binutils..."

mkdir -p build-binutils
cd build-binutils

../binutils-${BINUTILS_VER}/configure \
    --target=${TARGET} \
    --prefix=${PREFIX} \
    --with-sysroot \
    --disable-nls \
    --disable-werror

make -j"$(nproc)"
make install

cd ..

echo
echo -e "${BLUE}[INFO]${NC} Building GCC..."

mkdir -p build-gcc
cd build-gcc

../gcc-${GCC_VER}/configure \
    --target=${TARGET} \
    --prefix=${PREFIX} \
    --disable-nls \
    --enable-languages=c,c++ \
    --without-headers

make -j"$(nproc)" all-gcc
make -j"$(nproc)" all-target-libgcc

make install-gcc
make install-target-libgcc

cd ..

echo
echo -e "${BLUE}[INFO]${NC} Updating PATH..."

if [ -n "${ZSH_VERSION:-}" ]; then
    SHELL_RC="${HOME}/.zshrc"
else
    SHELL_RC="${HOME}/.bashrc"
fi

grep -q "${PREFIX}/bin" "$SHELL_RC" 2>/dev/null || \
echo "export PATH=\"${PREFIX}/bin:\$PATH\"" >> "$SHELL_RC"

export PATH="${PREFIX}/bin:$PATH"

echo
echo "=================================================="
echo "Installation Complete"
echo "=================================================="
echo

check_tool x86_64-elf-gcc
check_tool x86_64-elf-g++
check_tool x86_64-elf-ld

echo

echo -e "${GREEN}[SUCCESS]${NC} Cross toolchain installed successfully."

echo
echo "You may need to restart your terminal or run:"
echo
echo "source ${SHELL_RC}"
echo

x86_64-elf-gcc --version
echo
x86_64-elf-g++ --version
echo
x86_64-elf-ld --version
