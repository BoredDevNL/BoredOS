# Copyright (c) 2023-2026 Christiaan (chris@boreddev.nl)
# This software is released under the GNU General Public License v3.0. See LICENSE file for details.
# This header needs to maintain in any file it is present in, as per the GPL license terms.

export MAKEFLAGS += -j4

CC = x86_64-elf-gcc
LD = x86_64-elf-ld
NASM = nasm
XORRISO = xorriso

KERNEL_DIRS = arch core dev drivers fs graphics input mem net sys
BUILD_DIR = build
ISO_DIR = iso_root
FONT_SRC := external/bfonts/fonts
KERNEL_ELF = $(BUILD_DIR)/boredos.elf
ISO_IMAGE = boredos.iso

# Package-based applications/assets
PACKAGES = kilo lua bfonts nova doomgeneric bart serenityicons tcc netutils bearssl

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
                ! -path '*/third_party/lwip/netif/slipif.c')
ASM_SOURCES := $(shell find $(KERNEL_DIRS) -type f -name '*.asm')

OBJ_FILES := $(patsubst %.c, $(BUILD_DIR)/%.o, $(C_SOURCES)) \
             $(patsubst %.asm, $(BUILD_DIR)/%.o, $(ASM_SOURCES))

INCLUDE_DIRS := $(shell find $(KERNEL_DIRS) -type d)
INCLUDES := $(patsubst %, -I%, $(INCLUDE_DIRS))

CFLAGS = -g -O2 -pipe -Wall -Wextra -std=gnu11 -ffreestanding \
         -fno-stack-protector -fno-stack-check -fno-lto -fPIE \
         -m64 -march=x86-64 -msse -msse2 -mstackrealign -mno-red-zone \
         $(INCLUDES)

LDFLAGS = -m elf_x86_64 -nostdlib -static -pie --no-dynamic-linker \
          -z text -z max-page-size=0x1000 -T linker.ld

NASMFLAGS = -f elf64

LIMINE_VERSION = 11.4.1
LIMINE_URL_BASE = https://github.com/limine-bootloader/limine/raw/v$(LIMINE_VERSION)

HOST_OS := $(shell uname -s 2>/dev/null || echo Windows)

.PHONY: all clean run run-hd limine-setup run-windows run-mac run-linux run-hd-mac run-hd-windows run-hd-linux userland

all:
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
		git clone https://github.com/limine-bootloader/limine.git --branch=v$(LIMINE_VERSION)-binary --depth=1 limine; \
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


BEARSSL_LIB = external/bearssl/libbearssl.a

$(BEARSSL_LIB): build/sdk
	$(call PRINT_STEP,BUILDING BEARSSL)
	$(MAKE) -C external/bearssl CC=$(CC) AR=$(AR) BOREDOS_SDK=$(abspath build/sdk)
	@printf "$(GREEN)[OK]$(RESET) BearSSL built: $@\n"

$(KERNEL_ELF): $(OBJ_FILES) $(BEARSSL_LIB)
	$(call PRINT_STEP,LINKING KERNEL)
	@printf "$(YELLOW)[LD]$(RESET) Linking kernel ELF: $@\n"
	$(LD) $(LDFLAGS) -o $@ $(OBJ_FILES) $(BEARSSL_LIB)
	@printf "$(GREEN)[OK]$(RESET) Kernel ELF built: $@\n"

external-fetch:
	$(call PRINT_STEP,FETCHING EXTERNAL REPOSITORIES)
	@sh tools/fetch_external.sh

build/sdk: external-fetch
	$(call PRINT_STEP,BUILDING BOREDOS SDK (LIBC))
	@mkdir -p build/sdk
	$(MAKE) -C external/libc SDK_DIR=$(abspath build/sdk) install

userland: build/sdk
	$(call PRINT_STEP,BUILDING USERERLAND APPLICATIONS)
	@mkdir -p build/userland/bin
	$(MAKE) -C external/bsh BOREDOS_SDK=$(abspath build/sdk) DESTDIR=$(abspath build/userland/bin)
	$(MAKE) -C external/coreutils BOREDOS_SDK=$(abspath build/sdk) DESTDIR=$(abspath build/userland/bin)
	$(MAKE) -C external/nova BOREDOS_SDK=$(abspath build/sdk) DESTDIR=$(abspath build/userland/bin)
	$(MAKE) -C external/kilo BOREDOS_SDK=$(abspath build/sdk) DESTDIR=$(abspath build/userland/bin)
	$(MAKE) -C external/boredos_install BOREDOS_SDK=$(abspath build/sdk) DESTDIR=$(abspath build/userland/bin)
	$(MAKE) -C external/lua BOREDOS_SDK=$(abspath build/sdk) DESTDIR=$(abspath build/userland/bin)
	$(MAKE) -C external/tcc BOREDOS_SDK=$(abspath build/sdk) DESTDIR=$(abspath build/userland/bin)
	$(MAKE) -C external/netutils BOREDOS_SDK=$(abspath build/sdk) DESTDIR=$(abspath build/userland/bin)
	$(MAKE) -C external/doomgeneric BOREDOS_SDK=$(abspath build/sdk) DESTDIR=$(abspath build/userland/bin)
	$(MAKE) -C external/bpm BOREDOS_SDK=$(abspath build/sdk) DESTDIR=$(abspath build/userland/bin)
	@printf "$(GREEN)[OK]$(RESET) Userland build complete.\n"

.PHONY: packages
packages: build/sdk $(BEARSSL_LIB)
	$(call PRINT_STEP,BUILDING BOREDOS PACKAGES)
	@for pkg in $(PACKAGES); do \
		printf "$(YELLOW)[PACKAGES]$(RESET) Building package $$pkg...\n"; \
		$(MAKE) -C external/$$pkg BOREDOS_SDK=$(abspath build/sdk) bup || exit 1; \
	done

$(BUILD_DIR)/initrd.tar: $(KERNEL_ELF) userland packages
	$(call PRINT_STEP,BUILDING INITRD)
	@printf "$(YELLOW)[INITRD]$(RESET) Cleaning previous initrd directory...\n"
	rm -rf $(BUILD_DIR)/initrd

	@printf "$(YELLOW)[INITRD]$(RESET) Creating directory structure...\n"
	mkdir -p $(BUILD_DIR)/initrd/bin
	mkdir -p $(BUILD_DIR)/initrd/Library/images/Wallpapers
	mkdir -p $(BUILD_DIR)/initrd/Library/images/icons/serenityicons/16x16
	mkdir -p $(BUILD_DIR)/initrd/Library/images/icons/serenityicons/32x32
	mkdir -p $(BUILD_DIR)/initrd/Library/Fonts/Emoji
	mkdir -p $(BUILD_DIR)/initrd/Library/DOOM
	mkdir -p $(BUILD_DIR)/initrd/Library/conf
	mkdir -p $(BUILD_DIR)/initrd/Library/bsh
	mkdir -p $(BUILD_DIR)/initrd/Library/BWM/Wallpaper
	mkdir -p $(BUILD_DIR)/initrd/Library/art
	mkdir -p $(BUILD_DIR)/initrd/Library/images/branding
	mkdir -p $(BUILD_DIR)/initrd/docs
	mkdir -p $(BUILD_DIR)/initrd/boot
	mkdir -p $(BUILD_DIR)/initrd/mnt
	mkdir -p $(BUILD_DIR)/initrd/dev
	mkdir -p $(BUILD_DIR)/initrd/root/Desktop
	mkdir -p $(BUILD_DIR)/initrd/root/Pictures
	mkdir -p $(BUILD_DIR)/initrd/root/Documents
	mkdir -p $(BUILD_DIR)/initrd/root/Downloads
	mkdir -p $(BUILD_DIR)/initrd/etc

	mkdir -p $(BUILD_DIR)/initrd/usr/lib/tcc/include
	mkdir -p $(BUILD_DIR)/initrd/usr/local/include
	mkdir -p $(BUILD_DIR)/initrd/usr/include/sys
	mkdir -p $(BUILD_DIR)/initrd/usr/include/libc
	mkdir -p $(BUILD_DIR)/initrd/usr/lib

	@printf "$(YELLOW)[COPY]$(RESET) Limine binaries + kernel for installer...\n"
	@if [ -f limine/BOOTX64.EFI ]; then cp limine/BOOTX64.EFI    $(BUILD_DIR)/initrd/boot/; fi
	@if [ -f limine/BOOTIA32.EFI ];    then cp limine/BOOTIA32.EFI   $(BUILD_DIR)/initrd/boot/; fi
	@if [ -f limine/limine-bios.sys ]; then cp limine/limine-bios.sys $(BUILD_DIR)/initrd/boot/; fi
	@cp $(KERNEL_ELF) $(BUILD_DIR)/initrd/boot/boredos.elf

	@printf "$(YELLOW)[STAGE]$(RESET) Invoking modular repository installations...\n"
	$(MAKE) -C external/bsh BOREDOS_SDK=$(abspath build/sdk) DESTDIR=$(abspath $(BUILD_DIR)/initrd) install
	$(MAKE) -C external/coreutils BOREDOS_SDK=$(abspath build/sdk) DESTDIR=$(abspath $(BUILD_DIR)/initrd) install
	$(MAKE) -C external/boredos_install BOREDOS_SDK=$(abspath build/sdk) DESTDIR=$(abspath $(BUILD_DIR)/initrd) install
	$(MAKE) -C external/bpm BOREDOS_SDK=$(abspath build/sdk) DESTDIR=$(abspath $(BUILD_DIR)/initrd) install
	@for pkg in $(PACKAGES); do \
		$(MAKE) -C external/$$pkg BOREDOS_SDK=$(abspath build/sdk) DESTDIR=$(abspath $(BUILD_DIR)/initrd) install || exit 1; \
	done

	@printf "$(YELLOW)[STAGE]$(RESET) Staging package .bup files on Live CD...\n"
	@mkdir -p $(BUILD_DIR)/initrd/usr/share/packages
	@for pkg in $(PACKAGES); do \
		cp external/$$pkg/build/$$pkg.bup $(BUILD_DIR)/initrd/usr/share/packages/ || exit 1; \
	done
	@printf "$(YELLOW)[PACKAGES]$(RESET) Generating exclusions list...\n"
	@bash tools/gen_excludes.sh $(abspath $(BUILD_DIR)/initrd)

	@printf "$(YELLOW)[COPY]$(RESET) Staging SDK development environment files in initrd...\n"
	@cp build/sdk/lib/libc.a $(BUILD_DIR)/initrd/usr/lib/
	@cp build/sdk/lib/libc.a $(BUILD_DIR)/initrd/usr/lib/libboredos.a
	@cp build/sdk/lib/libc.a $(BUILD_DIR)/initrd/usr/lib/libm.a
	@x86_64-elf-strip -S $(BUILD_DIR)/initrd/usr/lib/libc.a
	@x86_64-elf-strip -S $(BUILD_DIR)/initrd/usr/lib/libboredos.a
	@x86_64-elf-strip -S $(BUILD_DIR)/initrd/usr/lib/libm.a
	@cp build/sdk/lib/crt0.o $(BUILD_DIR)/initrd/usr/lib/crt0.o
	@cp build/sdk/lib/crt1.o $(BUILD_DIR)/initrd/usr/lib/crt1.o
	@cp build/sdk/lib/crti.o $(BUILD_DIR)/initrd/usr/lib/crti.o
	@cp build/sdk/lib/crtn.o $(BUILD_DIR)/initrd/usr/lib/crtn.o
	@cp -r build/sdk/include/. $(BUILD_DIR)/initrd/usr/include/


	@printf "$(YELLOW)[COPY]$(RESET) Branding assets...\n"
	@cp -r branding/* $(BUILD_DIR)/initrd/Library/images/branding/

	@printf "$(YELLOW)[COPY]$(RESET) bsh configuration...\n"
	@if [ -f library/bsh/bshrc ]; then printf "  -> bshrc\n"; cp library/bsh/bshrc $(BUILD_DIR)/initrd/Library/bsh/; fi
	@if [ -f library/bsh/startup.bsh ]; then printf "  -> startup.bsh\n"; cp library/bsh/startup.bsh $(BUILD_DIR)/initrd/Library/bsh/; fi
	@if [ -f library/bsh/boot.bsh ]; then printf "  -> boot.bsh\n"; cp library/bsh/boot.bsh $(BUILD_DIR)/initrd/Library/bsh/; fi
	@if [ -f library/conf/sysfetch.cfg ]; then printf "  -> sysfetch.cfg\n"; cp library/conf/sysfetch.cfg $(BUILD_DIR)/initrd/Library/conf/; fi
	@if [ -f library/conf/taskbar.conf ]; then printf "  -> taskbar.conf\n"; cp library/conf/taskbar.conf $(BUILD_DIR)/initrd/Library/conf/; fi
	@if [ -f library/conf/wallpaper.conf ]; then printf "  -> wallpaper.conf\n"; cp library/conf/wallpaper.conf $(BUILD_DIR)/initrd/Library/conf/; fi
	@mkdir -p $(BUILD_DIR)/initrd/etc/nova
	@if [ -f library/conf/nova.conf ]; then printf "  -> nova.conf\n"; cp library/conf/nova.conf $(BUILD_DIR)/initrd/etc/nova/; fi

	@printf "$(YELLOW)[COPY]$(RESET) Copying Freedoom assets...\n"
	@if [ -f external/doomgeneric/freedoom1.wad ]; then \
		printf "  -> freedoom1.wad -> Library/DOOM/doom1.wad\n"; \
		cp external/doomgeneric/freedoom1.wad $(BUILD_DIR)/initrd/Library/DOOM/freedoom1.wad; \
	fi


	@printf "$(YELLOW)[COPY]$(RESET) ASCII art...\n"
	@if [ -f library/art/boredos.txt ]; then printf "  -> boredos.txt\n"; cp library/art/boredos.txt $(BUILD_DIR)/initrd/Library/art/; fi

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
	@if [ -f README.md ]; then printf "  -> README.md\n"; cp README.md $(BUILD_DIR)/initrd/; fi
	@if [ -f LICENSE ]; then printf "  -> LICENSE\n"; cp LICENSE $(BUILD_DIR)/initrd/; fi
	@if [ -f limine.conf ]; then printf "  -> limine.conf\n"; cp limine.conf $(BUILD_DIR)/initrd/; fi
	
	@printf "$(YELLOW)[TAR]$(RESET) Creating initrd.tar...\n"
	cd $(BUILD_DIR)/initrd && COPYFILE_DISABLE=1 tar --exclude="._*" -cf ../initrd.tar *
	@printf "$(GREEN)[OK]$(RESET) Initrd created: $(BUILD_DIR)/initrd.tar\n"

$(BUILD_DIR)/initrd.tar.lz4: $(BUILD_DIR)/initrd.tar
	@printf "$(YELLOW)[LZ4]$(RESET) Compressing initrd.tar...\n"
	lz4 -f -9 --content-size $(BUILD_DIR)/initrd.tar $(BUILD_DIR)/initrd.tar.lz4
	@printf "$(GREEN)[OK]$(RESET) LZ4 compressed initrd created: $(BUILD_DIR)/initrd.tar.lz4\n"

$(ISO_IMAGE): $(KERNEL_ELF) $(BUILD_DIR)/initrd.tar.lz4 limine.conf limine-setup
	$(call PRINT_STEP,CREATING ISO IMAGE)
	@printf "$(YELLOW)[ISO]$(RESET) Cleaning previous ISO root...\n"
	rm -rf $(ISO_DIR)

	@printf "$(YELLOW)[ISO]$(RESET) Creating ISO directory structure...\n"
	mkdir -p $(ISO_DIR)
	mkdir -p $(ISO_DIR)/EFI/BOOT
	
	@printf "$(YELLOW)[COPY]$(RESET) Kernel ELF...\n"
	cp $(KERNEL_ELF) $(ISO_DIR)/

	@printf "$(YELLOW)[COPY]$(RESET) Limine config...\n"
	cp limine.conf $(ISO_DIR)/
	
	@printf "$(YELLOW)[COPY]$(RESET) Initrd...\n"
	cp $(BUILD_DIR)/initrd.tar.lz4 $(ISO_DIR)/

	@printf "$(YELLOW)[CONFIG]$(RESET) Adding initrd module path...\n"
	printf "    module_path: boot():/initrd.tar.lz4\n" >> $(ISO_DIR)/limine.conf
	
	@printf "$(YELLOW)[COPY]$(RESET) Optional splash image...\n"
	@if [ -f branding/splash.jpg ]; then printf "  -> splash.jpg\n"; cp branding/splash.jpg $(ISO_DIR)/splash.jpg; else printf "  -> no splash.jpg found\n"; fi
	
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
	@for dir in external/*; do \
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
else ifeq ($(HOST_OS),Linux)
run: run-linux
run-hd: run-hd-linux
else
run: run-windows
run-hd: run-hd-windows
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
		-audiodev coreaudio,id=audio0,out.frequency=48000 -machine pcspk-audiodev=audio0 \
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
		-drive file=disk.img,format=raw,if=none,id=disk1 -device ide-hd,bus=ahci.1,drive=disk1 \
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
