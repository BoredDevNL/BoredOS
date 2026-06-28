# Creating Custom Applications & Adding External Repositories

In BoredOS, all userland applications, graphical shells, network diagnostics, and tools reside inside isolated external repositories under `/contrib/`. 

This guide details:
1. How to develop new applications within existing repositories.
2. What conditions warrant creating a **new** standalone repository.
3. A step-by-step tutorial on registering and integrating a new repository.

---

## 1. Developing Apps in Existing Repositories

If your new tool is small, lightweight, or naturally fits into an existing logical group, you should develop it directly inside one of the current external repositories. (See the repositories under the [BoredOS account.](https://github.com/BoredOS))

### Adding your application code:
1. Navigate to the appropriate subdirectory in the corresponding `contrib/` repo (e.g. `contrib/coreutils/src/` or `contrib/nova/apps/`).
2. Add your `.c` source code file.
3. The sub-makefiles in these repositories utilize wildcards (e.g. `$(wildcard src/*.c)`) to automatically scan and compile new files. If your new file is not covered, simply append it to the `SOURCES` variable in the repository's Makefile.
4. Stage it in the root `Makefile`'s `$(BUILD_DIR)/initrd.tar` rule to make sure it gets copied into the `initrd` filesystem (e.g. `/bin/` or `/Library/`).

---

## 2. What Warrants a NEW Repository?

Developers should avoid bloating existing folders. Creating a **new, dedicated external repository** is highly recommended under the following guidelines:

1. **Large Standalone Scope**: Any large application featuring significant logic (e.g. a game, a terminal emulator, or a custom database engine) should have its own repository.
2. **Third-Party Ports**: Any imported third-party open-source codebase (e.g. `lua`, `tcc`, or a standard library) **must** reside in its own isolated repository.
3. **Licensing Isolation**: BoredOS core components use the **GNU GPL v3.0**. If you are porting software that is licensed under a different standard (such as the **MIT License**, **BSD 2-Clause**, or **LGPL**), you **must** isolate it in its own repository with its own license file to maintain licensing compliance.
4. **Asset & Media Footprint**: Repositories containing thousands of binary/media assets (such as the `bart` wallpapers or the `serenityicons` icon sets) must be isolated to keep the main code repository small, quick to stage, and easy to clone.

---

## 3. Integrating a New Repository: Step-by-Step

Let's assume you are creating a new GUI calculator app named `calc`. Here is how to create, register, and integrate it into the BoredOS build pipeline:

### Step 1: Create the Local Repository
Create the directory structure under `/contrib/calc/` and add your source files:
```sh
mkdir -p contrib/calc/src
```

### Step 2: Write an Autonomic Standalone Makefile
Every external repository Makefile must support both **Integrated Builds** (within the BoredOS workspace) and **Standalone Builds** (cloned in isolation). 

Write `contrib/calc/Makefile`:
```makefile
# Default to local build/sdk if not passed by root Makefile
BOREDOS_SDK ?= $(abspath build/sdk)
DESTDIR ?= $(abspath build/bin)

# Setup Compiler flags using BOREDOS_SDK
CC = x86_64-elf-gcc
CFLAGS = -O2 -m64 -march=x86-64 -fno-stack-protector -fno-stack-check -fno-lto -fno-pie -ffreestanding -nostdlib -static -no-pie
INCLUDES = -I$(BOREDOS_SDK)/include

# Source files
SOURCES = $(wildcard src/*.c)
OBJECTS = $(SOURCES:src/%.c=obj/%.o)
BINARY = calc.elf

all: $(BINARY)

# Standalone build automatic bootstrap
$(BOREDOS_SDK)/lib/libc.a:
	@printf "Standalone build detected. Fetching C SDK (libc)...\n"
	@git clone https://github.com/boredos/libc.git build/libc-sdk
	@$(MAKE) -C build/libc-sdk SDK_DIR=$(BOREDOS_SDK) install
	@rm -rf build/libc-sdk

# Compile C source files
obj/%.o: src/%.c | $(BOREDOS_SDK)/lib/libc.a
	@mkdir -p obj
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# Link the userland executable
$(BINARY): $(OBJECTS)
	x86_64-elf-ld -m elf_x86_64 -nostdlib -static -no-pie -Ttext=0x40000000 \
		--no-dynamic-linker -z text -z max-page-size=0x1000 -e _start \
		-L$(BOREDOS_SDK)/lib $(BOREDOS_SDK)/lib/crt0.o $(OBJECTS) -lc -o $@

install: $(BINARY)
	mkdir -p $(DESTDIR)/bin
	cp $(BINARY) $(DESTDIR)/bin/

clean:
	rm -rf obj $(BINARY) build
```

### Step 3: Register as a Git Submodule
To integrate your repository into the standard BoredOS distribution build:
1. Publish your new repository on GitHub **under your own personal user account** (e.g., `https://github.com/<your-github-username>/calc.git`).
2. Open a **Pull Request (PR)** on the parent BoredOS repository to add your repository as a submodule:
   ```sh
   git submodule add https://github.com/<your-github-username>/calc.git contrib/calc
   ```

### Step 4: Configure the Root Makefile Integration
Now, wire the new repository into the top-level [Makefile](file:///Users/chris/BoredOS/Makefile):

1. **Userland Build Rules**:
   Find the `userland:` target and append the calc compilation line:
   ```makefile
   userland: build/sdk
   	...
   	$(MAKE) -C contrib/calc BOREDOS_SDK=$(abspath build/sdk) DESTDIR=$(abspath build/userland/bin)
   ```

2. **Initrd Copy & Staging Rules**:
   Find the `$(BUILD_DIR)/initrd.tar:` target and add the modular installation hook:
   ```makefile
   $(BUILD_DIR)/initrd.tar: $(KERNEL_ELF) userland
   	...
   	@printf "$(YELLOW)[STAGE]$(RESET) Invoking modular repository installations...\n"
   	...
   	$(MAKE) -C contrib/calc BOREDOS_SDK=$(abspath build/sdk) DESTDIR=$(abspath $(BUILD_DIR)/initrd) install
   ```

3. **Clean Rule Requirements**:
   - **No edits are required in the root Makefile for cleaning!**
   - The root Makefile `clean` target dynamically scans the `contrib/` folder and automatically invokes `$(MAKE) -C "$$dir" clean` on any subdirectory that contains a `Makefile`.
   - You **must** ensure that your repository's Makefile implements a standard, working `clean:` target (as written in Step 2) so that `make clean` executes successfully.
   ```

---

## 4. Testing Your New Repository

Once integrated, run the verification build and launch the QEMU emulator:
```sh
make clean && make run
```
Your new repository will automatically be resolved, compiled standalone using the shared C SDK, packaged cleanly into the `/bin` directory of your bootable initrd filesystem, and available on the desktop terminal console!

---

## 5. BoredOS Application Data Philosophy (/Library/AppData)

To maintain a clean, modular, and package-manageable filesystem, BoredOS enforces a strict directory standard:

### Standardized Application Assets & Configs
Every application **MUST** store its private assets, local resources, and configuration files inside a dedicated subdirectory under `/Library/AppData/`:
- **Path format**: `/Library/AppData/<app_identifier>/`
- **Application Identifier**: The subdirectory name must strictly match the package `name` field defined in the application's `MANIFEST.toml` (typically using reverse-domain notation, e.g., `org.boredos.nova`, `org.boredos.doomgeneric`).

### Desktop Entries & Shortcuts
Applications offering start menu integration must deploy their `.desktop` file to:
- `/Library/AppData/<app_identifier>/<app_name>.desktop`

### Why this is mandatory:
1. **Packaging Consistency**: The BoredOS Package Manager (`bpm`) relies on the name in `MANIFEST.toml` to automatically install and isolate configs, assets, and desktop shortcuts to `/Library/AppData/<name>`.
2. **Conflict Resolution**: Storing files in application-specific sandboxes prevents namespace collision. No files should be written directly to root-level `/Library`, `/etc`, or `/usr/share` by individual applications.
3. **Clean Uninstalls**: Because files are organized inside a single directory matching the package name, `bpm` can safely clean up all application state during removal. Mismatches in directory naming between the compile-time Make targets and the `MANIFEST.toml` manifest lead to duplicate shortcut entries and missing icons in the start menu.