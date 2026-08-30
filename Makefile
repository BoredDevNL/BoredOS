# Copyright (c) 2023-2026 Christiaan (chris@boreddev.nl)
# This software is released under the GNU General Public License v3.0. See LICENSE file for details.
# This header needs to maintain in any file it is present in, as per the GPL license terms.

export MAKEFLAGS += -j4

COMPILER_IN_PATH := $(shell command -v x86_64-boredos-gcc >/dev/null 2>&1 && echo yes)

TOOLCHAIN_PATH := $(shell \
	if [ -n "$(COMPILER_IN_PATH)" ]; then \
		echo ""; \
	elif [ -x /opt/boredos-toolchain/bin/x86_64-boredos-gcc ]; then \
		echo "/opt/boredos-toolchain/bin"; \
	elif [ -x $(HOME)/boredos-toolchain/bin/x86_64-boredos-gcc ]; then \
		echo "$(HOME)/boredos-toolchain/bin"; \
	fi)

ifneq ($(TOOLCHAIN_PATH),)
export PATH := $(TOOLCHAIN_PATH):$(PATH)

ifeq ($(COMPILER_IN_PATH),)
.SUFFIXES:
.DEFAULT:
	@PATH="$(TOOLCHAIN_PATH):$(PATH)" $(MAKE) -f $(firstword $(MAKEFILE_LIST)) $(MAKECMDGOALS)

all:
	@PATH="$(TOOLCHAIN_PATH):$(PATH)" $(MAKE) -f $(firstword $(MAKEFILE_LIST)) all

REEXEC := 1
endif
endif

ifndef REEXEC

CC = x86_64-boredos-gcc
LD = x86_64-boredos-ld
AR = x86_64-boredos-ar
STRIP = x86_64-boredos-strip
NASM = nasm
XORRISO = xorriso

KERNEL_DIRS = arch core dev drivers fs graphics input mem net sys
BUILD_DIR = build
ISO_DIR = iso_root
FONT_SRC := usr/bfonts/fonts
KERNEL_ELF = $(BUILD_DIR)/boredos.elf
ISO_IMAGE = boredos.iso

# Package-based applications/assets
PACKAGES = kilo lua bfonts nova doomgeneric bart serenityicons tcc netutils bearssl tinygl btvi kirc

BLUE  = \033[1;34m
GREEN = \033[1;32m
YELLOW= \033[1;33m
RESET = \033[0m

define PRINT_STEP
	@printf ""
	@printf "\n$(BLUE)============================================================$(RESET)\n"
	@printf "$(BLUE)== %s$(RESET)\n" "$(1)"
	@printf "$(BLUE)============================================================$(RESET)\n"
endef

C_SOURCES := $(shell find $(KERNEL_DIRS) -type f -name '*.c' \
                ! -path '*/third_party/lwip/netif/slipif.c' \
                ! -path 'fs/vendor/*' \
                ! -path '*/fs/vendor/*')
ASM_SOURCES := $(shell find $(KERNEL_DIRS) -type f -name '*.asm' \
                ! -path 'fs/vendor/*' \
                ! -path '*/fs/vendor/*')

OBJ_FILES := $(patsubst %.c, $(BUILD_DIR)/%.o, $(C_SOURCES)) \
             $(patsubst %.asm, $(BUILD_DIR)/%.o, $(ASM_SOURCES))

INCLUDE_DIRS := $(shell find $(KERNEL_DIRS) -type d \
                ! -path 'fs/vendor/*' \
                ! -path '*/fs/vendor/*')
INCLUDES := $(patsubst %, -I%, $(INCLUDE_DIRS))

# Detect clang wrapper (FreeBSD) vs freestanding GCC cross (macOS/Linux)
CC_IS_CLANG := $(shell $(CC) --version 2>/dev/null | grep -q clang && echo 1)
ifeq ($(CC_IS_CLANG),1)
  TOOLCHAIN_FLAGS = -DBOREDOS_SYS_TIMEVAL
endif

CFLAGS = -g -O2 -pipe -Wall -Wextra -std=gnu11 -ffreestanding \
         -fno-stack-protector -fno-stack-check -fno-lto -fPIE \
         -m64 -march=x86-64 -msse -msse2 -mstackrealign -mno-red-zone \
         -MMD -MP \
         $(TOOLCHAIN_FLAGS) $(INCLUDES) \
         -Ifs/vendor/lwext4/include -Ifs/vendor/lwext4/include/misc

-include $(OBJ_FILES:.o=.d)

LDFLAGS = -m elf_x86_64 -nostdlib -static -pie --no-dynamic-linker \
          -z text -z max-page-size=0x1000 -T linker.ld

NASMFLAGS = -f elf64

LIMINE_VERSION = 11.4.1
LIMINE_URL_BASE = https://github.com/limine-bootloader/limine/raw/v$(LIMINE_VERSION)

HOST_OS := $(shell uname -s 2>/dev/null || echo Windows)

.PHONY: all clean run run-hd limine-setup run-windows run-mac run-linux run-hd-mac run-hd-windows run-hd-linux userland usr-fetch

all: usr-fetch
	$(call PRINT_STEP,STARTING BOREDOS BUILD)
	$(MAKE) $(ISO_IMAGE)
	$(call PRINT_STEP,BUILD COMPLETE)

$(BUILD_DIR):
	$(call PRINT_STEP,CREATING BUILD DIRECTORY)
	mkdir -p $(BUILD_DIR)

limine-setup:
	$(call PRINT_STEP,SETTING UP LIMINE)
	@if [ ! -f limine/limine-bios.sys ]; then \
		printf "$(YELLOW)[LIMINE] Limine binaries missing or invalid. Cloning v$(LIMINE_VERSION)-binary...$(RESET)\n"; \
		rm -rf limine; \
		git clone https://github.com/limine-bootloader/limine.git --branch=v$(LIMINE_VERSION)-binary limine; \
	else \
		printf "$(YELLOW)[LIMINE] Existing Limine binaries found.$(RESET)\n"; \
	fi
	@if [ ! -f core/limine.h ]; then \
		printf "$(YELLOW)[LIMINE] Copying limine.h...$(RESET)\n"; \
		cp limine/limine.h core/limine.h; \
	else \
		printf "$(YELLOW)[LIMINE] limine.h already present.$(RESET)\n"; \
	fi
	@printf "$(YELLOW)[LIMINE] Building Limine host utility...$(RESET)\n"
	$(MAKE) -C limine
	@printf "$(GREEN)[OK] Limine setup complete.$(RESET)\n"

$(BUILD_DIR)/%.o: %.c | $(BUILD_DIR) limine-setup
	@printf "$(YELLOW)[CC]$(RESET) $< -> $@\n"
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/%.o: %.asm | $(BUILD_DIR)
	@printf "$(YELLOW)[ASM]$(RESET) $< -> $@\n"
	@mkdir -p $(dir $@)
	$(NASM) $(NASMFLAGS) $< -o $@


BEARSSL_LIB = usr/bearssl/libbearssl.a

$(BEARSSL_LIB): build/sdk
	$(call PRINT_STEP,BUILDING BEARSSL)
	$(MAKE) -C usr/bearssl CC=$(CC) AR=$(AR) BOREDOS_SDK=$(abspath build/sdk)
	@printf "$(GREEN)[OK]$(RESET) BearSSL built: $@\n"

# --- lwext4 static library ---
LWEXT4_SRC_DIR = fs/vendor/lwext4/src
LWEXT4_INC_DIR = fs/vendor/lwext4/include
LWEXT4_SRCS := $(wildcard $(LWEXT4_SRC_DIR)/*.c)
LWEXT4_OBJS := $(patsubst $(LWEXT4_SRC_DIR)/%.c, $(BUILD_DIR)/lwext4/%.o, $(LWEXT4_SRCS))
LWEXT4_LIB  = $(BUILD_DIR)/liblwext4.a

LWEXT4_CFLAGS = -g -O2 -pipe -std=gnu11 -ffreestanding \
                -fno-stack-protector -fno-stack-check -fno-lto -fPIE \
                -m64 -march=x86-64 -msse -msse2 -mstackrealign -mno-red-zone \
                -I$(LWEXT4_INC_DIR) -I$(LWEXT4_INC_DIR)/misc \
                -include fs/ext4_config.h \
                -Wno-unused-parameter -Wno-sign-compare -Wno-unused-variable \
                $(INCLUDES)

$(BUILD_DIR)/lwext4/%.o: $(LWEXT4_SRC_DIR)/%.c fs/ext4_config.h fs/inttypes.h | $(BUILD_DIR)
	@printf "$(YELLOW)[CC]$(RESET) $< -> $@\n"
	@mkdir -p $(dir $@)
	$(CC) $(LWEXT4_CFLAGS) -c $< -o $@

$(LWEXT4_LIB): $(LWEXT4_OBJS)
	$(call PRINT_STEP,BUILDING LWEXT4)
	$(AR) rcs $@ $(LWEXT4_OBJS)
	@printf "$(GREEN)[OK]$(RESET) lwext4 built: $@\n"

$(KERNEL_ELF): $(OBJ_FILES) $(BEARSSL_LIB) $(LWEXT4_LIB)
	$(call PRINT_STEP,LINKING KERNEL)
	@printf "$(YELLOW)[LD]$(RESET) Linking kernel ELF: $@\n"
	$(LD) $(LDFLAGS) -o $@ $(OBJ_FILES) $(BEARSSL_LIB) $(LWEXT4_LIB)
	@printf "$(GREEN)[OK]$(RESET) Kernel ELF built: $@\n"

usr-fetch:
	$(call PRINT_STEP,FETCHING EXTERNAL REPOSITORIES)
	@if git submodule status | grep -q "^-"; then \
		git submodule update --init --recursive; \
	fi

build/sdk: usr-fetch
	$(call PRINT_STEP,BUILDING BOREDOS SDK (MLIBC))
	@mkdir -p build/sdk
	@if [ ! -d build/mlibc_build ]; then \
		meson setup build/mlibc_build usr/mlibc \
			--cross-file tools/cross_file.txt \
			--prefix=$(abspath build/sdk) \
			--libdir=lib \
			-Ddefault_library=both \
			-Dheaders_only=false \
			-Dposix_option=enabled \
			-Dlinux_option=disabled \
			-Dglibc_option=disabled \
			-Dbsd_option=disabled; \
	fi
	ninja -C build/mlibc_build install
	@SYSROOT=$$(x86_64-boredos-gcc -print-sysroot 2>/dev/null); \
	if [ -n "$$SYSROOT" ] && [ -d "$$SYSROOT" ]; then \
		printf "$(GREEN)[SDK]$(RESET) Installing mlibc SDK to toolchain sysroot: $$SYSROOT\n"; \
		mkdir -p "$$SYSROOT/usr/include" "$$SYSROOT/usr/lib" "$$SYSROOT/lib"; \
		cp -R build/sdk/include/. "$$SYSROOT/usr/include/"; \
		cp -R build/sdk/lib/. "$$SYSROOT/usr/lib/"; \
		cp -R build/sdk/lib/. "$$SYSROOT/lib/"; \
	else \
		printf "$(YELLOW)[SDK]$(RESET) Native sysroot not found; skipping toolchain sysroot auto-population.\n"; \
	fi

userland: build/sdk
	$(call PRINT_STEP,BUILDING USERERLAND APPLICATIONS)
	@mkdir -p build/userland/bin
	$(MAKE) -C usr/bsh BOREDOS_SDK=$(abspath build/sdk) DESTDIR=$(abspath build/userland/bin)
	$(MAKE) -C usr/coreutils BOREDOS_SDK=$(abspath build/sdk) DESTDIR=$(abspath build/userland/bin)
	$(MAKE) -C usr/nova BOREDOS_SDK=$(abspath build/sdk) DESTDIR=$(abspath build/userland/bin)
	$(MAKE) -C usr/kilo BOREDOS_SDK=$(abspath build/sdk) DESTDIR=$(abspath build/userland/bin)
	$(MAKE) -C usr/boredos_install BOREDOS_SDK=$(abspath build/sdk) DESTDIR=$(abspath build/userland/bin)
	$(MAKE) -C usr/lua BOREDOS_SDK=$(abspath build/sdk) DESTDIR=$(abspath build/userland/bin)
	$(MAKE) -C usr/tcc BOREDOS_SDK=$(abspath build/sdk) DESTDIR=$(abspath build/userland/bin)
	$(MAKE) -C usr/netutils BOREDOS_SDK=$(abspath build/sdk) DESTDIR=$(abspath build/userland/bin)
	$(MAKE) -C usr/doomgeneric BOREDOS_SDK=$(abspath build/sdk) DESTDIR=$(abspath build/userland/bin)
	$(MAKE) -C usr/tinygl BOREDOS_SDK=$(abspath build/sdk) DESTDIR=$(abspath build/userland/bin)
	$(MAKE) -C usr/bpm BOREDOS_SDK=$(abspath build/sdk) DESTDIR=$(abspath build/userland/bin)
	$(MAKE) -C usr/btvi BOREDOS_SDK=$(abspath build/sdk) DESTDIR=$(abspath build/userland/bin)
	$(MAKE) -C usr/kirc BOREDOS_SDK=$(abspath build/sdk) DESTDIR=$(abspath build/userland/bin)

	@printf "$(GREEN)[OK]$(RESET) Userland build complete.\n"

.PHONY: packages
packages: build/sdk $(BEARSSL_LIB) userland
	$(call PRINT_STEP,BUILDING BOREDOS PACKAGES)
	@for pkg in $(PACKAGES); do \
		printf "$(YELLOW)[PACKAGES]$(RESET) Building package $$pkg...\n"; \
		$(MAKE) -C usr/$$pkg BOREDOS_SDK=$(abspath build/sdk) bup || exit 1; \
	done

$(BUILD_DIR)/initrd.tar: $(KERNEL_ELF) userland packages
	$(call PRINT_STEP,BUILDING INITRD)
	@printf "$(YELLOW)[INITRD]$(RESET) Cleaning previous initrd directory...\n"
	rm -rf $(BUILD_DIR)/initrd

	mkdir -p $(BUILD_DIR)/initrd
	cp -R base/. $(BUILD_DIR)/initrd/
	@find $(BUILD_DIR)/initrd -name .gitkeep -delete
	@find $(BUILD_DIR)/initrd -name .DS_Store -delete

	@printf "$(YELLOW)[COPY]$(RESET) Limine binaries + kernel for installer...\n"
	@if [ -f limine/BOOTX64.EFI ]; then cp limine/BOOTX64.EFI    $(BUILD_DIR)/initrd/boot/; fi
	@if [ -f limine/BOOTIA32.EFI ];    then cp limine/BOOTIA32.EFI   $(BUILD_DIR)/initrd/boot/; fi
	@if [ -f limine/limine-bios.sys ]; then cp limine/limine-bios.sys $(BUILD_DIR)/initrd/boot/; fi
	@cp $(KERNEL_ELF) $(BUILD_DIR)/initrd/boot/boredos.elf

	@printf "$(YELLOW)[STAGE]$(RESET) Invoking modular repository installations...\n"
	$(MAKE) -C usr/bsh BOREDOS_SDK=$(abspath build/sdk) DESTDIR=$(abspath $(BUILD_DIR)/initrd) install
	$(MAKE) -C usr/coreutils BOREDOS_SDK=$(abspath build/sdk) DESTDIR=$(abspath $(BUILD_DIR)/initrd) install
	$(MAKE) -C usr/boredos_install BOREDOS_SDK=$(abspath build/sdk) DESTDIR=$(abspath $(BUILD_DIR)/initrd) install
	$(MAKE) -C usr/bpm BOREDOS_SDK=$(abspath build/sdk) DESTDIR=$(abspath $(BUILD_DIR)/initrd) install
	@for pkg in $(PACKAGES); do \
		$(MAKE) -C usr/$$pkg BOREDOS_SDK=$(abspath build/sdk) DESTDIR=$(abspath $(BUILD_DIR)/initrd) install || exit 1; \
	done

	@printf "$(YELLOW)[STAGE]$(RESET) Staging package .bup files on Live CD...\n"
	@mkdir -p $(BUILD_DIR)/initrd/usr/share/packages
	@for pkg in $(PACKAGES); do \
		cp usr/$$pkg/build/$$pkg.bup $(BUILD_DIR)/initrd/usr/share/packages/ || exit 1; \
	done
	@printf "$(YELLOW)[PACKAGES]$(RESET) Generating exclusions list...\n"
	@bash tools/gen_excludes.sh $(abspath $(BUILD_DIR)/initrd)

	@printf "$(YELLOW)[COPY]$(RESET) Staging SDK development environment & shared runtime libraries in initrd...\n"
	@mkdir -p $(BUILD_DIR)/initrd/usr/lib $(BUILD_DIR)/initrd/lib $(BUILD_DIR)/initrd/usr/include
	@cp -R build/sdk/lib/. $(BUILD_DIR)/initrd/usr/lib/ 2>/dev/null || true
	@cp -R build/sdk/lib/. $(BUILD_DIR)/initrd/lib/ 2>/dev/null || true
	@if [ -f $(BUILD_DIR)/initrd/usr/lib/ld.so ]; then chmod +x $(BUILD_DIR)/initrd/usr/lib/ld.so; fi
	@if [ -f $(BUILD_DIR)/initrd/lib/ld.so ]; then chmod +x $(BUILD_DIR)/initrd/lib/ld.so; fi
	@cp -r build/sdk/include/. $(BUILD_DIR)/initrd/usr/include/

	@printf "$(YELLOW)[COPY]$(RESET) Documentation...\n"
	@for f in $$(find docs -name '*.md' 2>/dev/null); do \
		if [ -f "$$f" ]; then \
			printf "  -> $$f\n"; \
			dir=$$(dirname "$$f"); \
			mkdir -p $(BUILD_DIR)/initrd/"$$dir"; \
			cp "$$f" $(BUILD_DIR)/initrd/"$$dir"/; \
		fi \
	done

	@printf "$(YELLOW)[COPY]$(RESET) Root files...\n"
	@if [ -f LICENSE ]; then printf "  -> LICENSE\n"; mkdir -p $(BUILD_DIR)/initrd/docs; cp LICENSE $(BUILD_DIR)/initrd/docs/; fi
	@if [ -f base/boot/limine.conf ]; then printf "  -> limine.conf\n"; cp base/boot/limine.conf $(BUILD_DIR)/initrd/; fi
	
	@printf "$(YELLOW)[STRIP]$(RESET) Stripping ELF binaries to reduce initrd size...\n"
	@find $(BUILD_DIR)/initrd/bin $(BUILD_DIR)/initrd/usr/bin -name '*.elf' 2>/dev/null | while read f; do \
		printf "  stripping $$f\n"; \
		$(STRIP) --strip-unneeded "$$f" || true; \
	done
	@printf "$(GREEN)[STRIP]$(RESET) Done stripping binaries.\n"

	@printf "$(YELLOW)[TAR]$(RESET) Creating initrd.tar...\n"
	cd $(BUILD_DIR)/initrd && COPYFILE_DISABLE=1 tar --exclude="._*" -cf ../initrd.tar *
	@printf "$(GREEN)[OK]$(RESET) Initrd created: $(BUILD_DIR)/initrd.tar\n"

$(BUILD_DIR)/initrd.tar.lz4: $(BUILD_DIR)/initrd.tar
	@printf "$(YELLOW)[LZ4]$(RESET) Compressing initrd.tar...\n"
	lz4 -f -9 --content-size $(BUILD_DIR)/initrd.tar $(BUILD_DIR)/initrd.tar.lz4
	@printf "$(GREEN)[OK]$(RESET) LZ4 compressed initrd created: $(BUILD_DIR)/initrd.tar.lz4\n"

$(ISO_IMAGE): $(KERNEL_ELF) $(BUILD_DIR)/initrd.tar.lz4 base/boot/limine.conf limine-setup
	$(call PRINT_STEP,CREATING ISO IMAGE)
	@printf "$(YELLOW)[ISO]$(RESET) Cleaning previous ISO root...\n"
	rm -rf $(ISO_DIR)

	@printf "$(YELLOW)[ISO]$(RESET) Creating ISO directory structure...\n"
	mkdir -p $(ISO_DIR)
	mkdir -p $(ISO_DIR)/EFI/BOOT
	
	@printf "$(YELLOW)[COPY]$(RESET) Kernel ELF...\n"
	cp $(KERNEL_ELF) $(ISO_DIR)/

	@printf "$(YELLOW)[COPY]$(RESET) Limine config...\n"
	cp base/boot/limine.conf $(ISO_DIR)/
	
	@printf "$(YELLOW)[COPY]$(RESET) Initrd...\n"
	cp $(BUILD_DIR)/initrd.tar.lz4 $(ISO_DIR)/

	@printf "$(YELLOW)[COPY]$(RESET) Optional splash image...\n"
	@if [ -f base/boot/splash.jpg ]; then printf "  -> splash.jpg\n"; cp base/boot/splash.jpg $(ISO_DIR)/splash.jpg; else printf "  -> no splash.jpg found\n"; fi
	
	@printf "$(YELLOW)[COPY]$(RESET) Limine boot files...\n"
	cp limine/limine-bios.sys $(ISO_DIR)/
	cp limine/limine-bios-cd.bin $(ISO_DIR)/
	cp limine/limine-uefi-cd.bin $(ISO_DIR)/
	
	@printf "$(YELLOW)[COPY]$(RESET) EFI bootloaders...\n"
	cp limine/BOOTX64.EFI $(ISO_DIR)/EFI/BOOT/
	cp limine/BOOTIA32.EFI $(ISO_DIR)/EFI/BOOT/

	$(call PRINT_STEP,GENERATING BOOTABLE ISO)
	$(XORRISO) -as mkisofs -R -J -b limine-bios-cd.bin \
		-no-emul-boot -boot-load-size 4 -boot-info-table \
		--efi-boot limine-uefi-cd.bin \
		-efi-boot-part --efi-boot-image --protective-msdos-label \
		$(ISO_DIR) -o $(ISO_IMAGE)
	
	@printf "$(YELLOW)[LIMINE]$(RESET) Installing BIOS bootloader...\n"
	./limine/limine bios-install $(ISO_IMAGE)
	@printf "$(GREEN)[OK]$(RESET) ISO image ready: $(ISO_IMAGE)\n"

clean:
	$(call PRINT_STEP,CLEANING BUILD OUTPUT)
	rm -rf $(BUILD_DIR) $(ISO_DIR) $(ISO_IMAGE)
	@for dir in usr/*; do \
		if [ -d "$$dir" ] && [ -f "$$dir/Makefile" ]; then \
			$(MAKE) -C "$$dir" clean; \
		fi \
	done
	@printf "$(GREEN)[OK]$(RESET) Clean complete.\n"

disk.qcow2:
	$(call PRINT_STEP,CREATING 10GB EXPANDABLE DISK IMAGE)
	qemu-img create -f qcow2 disk.qcow2 10G

ifeq ($(HOST_OS),Darwin)
run: run-mac
run-hd: run-hd-mac
run-serial: run-serial-mac
run-hd-serial: run-hd-serial-mac
else ifeq ($(HOST_OS),Linux)
run: run-linux
run-hd: run-hd-linux
run-serial: run-serial-linux
run-hd-serial: run-hd-serial-linux
else
run: run-windows
run-hd: run-hd-windows
run-serial: run-serial-windows
run-hd-serial: run-hd-serial-windows
endif

run-windows: $(ISO_IMAGE) disk.qcow2
	$(call PRINT_STEP,RUNNING BOREDOS IN QEMU ON WINDOWS)
	qemu-system-x86_64 -m 4G -serial stdio -cdrom $< -boot d \
	    -smp 4 \
		-audiodev dsound,id=audio0 -machine pcspk-audiodev=audio0 \
		-device AC97,audiodev=audio0 \
		-vga std -global VGA.xres=1920 -global VGA.yres=1080 \
		-drive file=disk.qcow2,format=qcow2,file.locking=off 

run-mac: $(ISO_IMAGE) disk.qcow2
	$(call PRINT_STEP,RUNNING BOREDOS IN QEMU ON MACOS)
	qemu-system-x86_64 -m 4G -serial stdio -cdrom $< -boot d \
	    -smp 4 \
		-audiodev coreaudio,id=audio0,out.frequency=48000 \
		-device AC97,audiodev=audio0 \
		-vga std -global VGA.xres=1920 -global VGA.yres=1080 \
		-display cocoa,show-cursor=off \
		-device ahci,id=ahci -drive file=disk.qcow2,format=qcow2,if=none,id=disk0 -device ide-hd,bus=ahci.0,drive=disk0 \
		-cpu max

OVMF_CODE := /opt/homebrew/share/qemu/edk2-x86_64-code.fd
OVMF_VARS_TMPL := /opt/homebrew/share/qemu/edk2-i386-vars.fd
OVMF_VARS := edk2-vars.fd

ifeq ($(shell test -f $(OVMF_CODE) && echo 1),)
    OVMF_CODE := /usr/local/share/qemu/edk2-x86_64-code.fd
    OVMF_VARS_TMPL := /usr/local/share/qemu/edk2-i386-vars.fd
endif

$(OVMF_VARS):
	@if [ -f $(OVMF_VARS_TMPL) ]; then \
		printf "$(YELLOW)[UEFI]$(RESET) Creating local NVRAM vars...\n"; \
		cp $(OVMF_VARS_TMPL) $(OVMF_VARS); \
	fi

run-hd-mac: disk.qcow2 $(OVMF_VARS)
	$(call PRINT_STEP,BOOTING BOREDOS FROM HARD DRIVE ON MACOS)
	qemu-system-x86_64 -m 4G -serial stdio -boot c \
	    -smp 4 \
		-audiodev coreaudio,id=audio0,out.frequency=48000 -machine pcspk-audiodev=audio0 \
		-device AC97,audiodev=audio0 \
		-vga std -global VGA.xres=1920 -global VGA.yres=1080 \
		-display cocoa,show-cursor=off \
		-drive if=pflash,format=raw,readonly=on,file=$(OVMF_CODE) \
		-drive if=pflash,format=raw,file=$(OVMF_VARS) \
		-device ahci,id=ahci \
		-drive file=disk.qcow2,format=qcow2,if=none,id=disk0 -device ide-hd,bus=ahci.0,drive=disk0 \
		-cpu max

run-linux: $(ISO_IMAGE) disk.qcow2
	$(call PRINT_STEP,RUNNING BOREDOS IN QEMU ON LINUX)
	qemu-system-x86_64 -m 4G -serial stdio -cdrom $< -boot d \
	    -smp 4 \
		-audiodev pa,id=audio0 -machine pcspk-audiodev=audio0 \
		-device AC97,audiodev=audio0 \
		-vga std -global VGA.xres=1920 -global VGA.yres=1080 \
		-display gtk,show-cursor=off \
		-device ahci,id=ahci -drive file=disk.qcow2,format=qcow2,if=none,id=disk0 -device ide-hd,bus=ahci.0,drive=disk0 \
		-cpu max

run-hd-windows: disk.qcow2
	$(call PRINT_STEP,BOOTING BOREDOS FROM HARD DRIVE ON WINDOWS)
	qemu-system-x86_64 -m 4G -serial stdio -boot c \
	    -smp 4 \
		-audiodev dsound,id=audio0 -machine pcspk-audiodev=audio0 \
		-device AC97,audiodev=audio0 \
		-vga std -global VGA.xres=1920 -global VGA.yres=1080 \
		-device ahci,id=ahci \
		-drive file=disk.qcow2,format=qcow2,if=none,id=disk0 -device ide-hd,bus=ahci.0,drive=disk0 \
		-cpu max

run-hd-linux: disk.qcow2 $(OVMF_VARS)
	$(call PRINT_STEP,BOOTING BOREDOS FROM HARD DRIVE ON LINUX)
	qemu-system-x86_64 -m 4G -serial stdio -boot c \
	    -smp 4 \
		-audiodev pa,id=audio0 -machine pcspk-audiodev=audio0 \
		-device AC97,audiodev=audio0 \
		-vga std -global VGA.xres=1920 -global VGA.yres=1080 \
		-display gtk,show-cursor=off \
		-drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE.fd \
		-drive if=pflash,format=raw,file=$(OVMF_VARS) \
		-device ahci,id=ahci \
		-drive file=disk.qcow2,format=qcow2,if=none,id=disk0 -device ide-hd,bus=ahci.0,drive=disk0 \
		-cpu max

run-serial-mac: $(ISO_IMAGE) disk.qcow2
	$(call PRINT_STEP,RUNNING BOREDOS OVER SERIAL IN QEMU ON MACOS)
	qemu-system-x86_64 -m 4G -serial stdio -serial file:kernel_debug.log -cdrom $< -boot d \
	    -smp 4 \
		-audiodev coreaudio,id=audio0,out.frequency=48000 -machine pcspk-audiodev=audio0 \
		-device AC97,audiodev=audio0 \
		-vga none -display none \
		-device ahci,id=ahci -drive file=disk.qcow2,format=qcow2,if=none,id=disk0 -device ide-hd,bus=ahci.0,drive=disk0 \
		-cpu max

run-hd-serial-mac: disk.qcow2 $(OVMF_VARS)
	$(call PRINT_STEP,BOOTING BOREDOS OVER SERIAL FROM HARD DRIVE ON MACOS)
	qemu-system-x86_64 -m 4G -serial stdio -serial file:kernel_debug.log -boot c \
	    -smp 4 \
		-audiodev coreaudio,id=audio0,out.frequency=48000 -machine pcspk-audiodev=audio0 \
		-device AC97,audiodev=audio0 \
		-vga none -display none \
		-drive if=pflash,format=raw,readonly=on,file=$(OVMF_CODE) \
		-drive if=pflash,format=raw,file=$(OVMF_VARS) \
		-device ahci,id=ahci \
		-drive file=disk.qcow2,format=qcow2,if=none,id=disk0 -device ide-hd,bus=ahci.0,drive=disk0 \
		-cpu max

run-serial-linux: $(ISO_IMAGE) disk.qcow2
	$(call PRINT_STEP,RUNNING BOREDOS OVER SERIAL IN QEMU ON LINUX)
	qemu-system-x86_64 -m 4G -serial stdio -serial file:kernel_debug.log -cdrom $< -boot d \
	    -smp 4 \
		-audiodev pa,id=audio0 -machine pcspk-audiodev=audio0 \
		-device AC97,audiodev=audio0 \
		-vga none -display none \
		-device ahci,id=ahci -drive file=disk.qcow2,format=qcow2,if=none,id=disk0 -device ide-hd,bus=ahci.0,drive=disk0 \
		-cpu max

run-hd-serial-linux: disk.qcow2 $(OVMF_VARS)
	$(call PRINT_STEP,BOOTING BOREDOS OVER SERIAL FROM HARD DRIVE ON LINUX)
	qemu-system-x86_64 -m 4G -serial stdio -serial file:kernel_debug.log -boot c \
	    -smp 4 \
		-audiodev pa,id=audio0 -machine pcspk-audiodev=audio0 \
		-device AC97,audiodev=audio0 \
		-vga none -display none \
		-drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE.fd \
		-drive if=pflash,format=raw,file=$(OVMF_VARS) \
		-device ahci,id=ahci \
		-drive file=disk.qcow2,format=qcow2,if=none,id=disk0 -device ide-hd,bus=ahci.0,drive=disk0 \
		-cpu max

run-serial-windows: $(ISO_IMAGE) disk.qcow2
	$(call PRINT_STEP,RUNNING BOREDOS OVER SERIAL IN QEMU ON WINDOWS)
	qemu-system-x86_64 -m 4G -serial stdio -serial file:kernel_debug.log -cdrom $< -boot d \
	    -smp 4 \
		-audiodev dsound,id=audio0 -machine pcspk-audiodev=audio0 \
		-device AC97,audiodev=audio0 \
		-vga none -display none \
		-drive file=disk.qcow2,format=qcow2,file.locking=off \
		-cpu max

run-hd-serial-windows: disk.qcow2
	$(call PRINT_STEP,BOOTING BOREDOS OVER SERIAL FROM HARD DRIVE ON WINDOWS)
	qemu-system-x86_64 -m 4G -serial stdio -serial file:kernel_debug.log -boot c \
	    -smp 4 \
		-audiodev dsound,id=audio0 -machine pcspk-audiodev=audio0 \
		-device AC97,audiodev=audio0 \
		-vga none -display none \
		-device ahci,id=ahci \
		-drive file=disk.qcow2,format=qcow2,if=none,id=disk0 -device ide-hd,bus=ahci.0,drive=disk0 \
		-cpu max

endif
