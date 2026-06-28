<div align="center">
  <h1>Example 01: Hello CLI</h1>
  <p><em>The absolute basics. Writing a terminal program.</em></p>
</div>

---

This example demonstrates the bare minimum structure of a BoredOS application that outputs text to the standard output (usually the Terminal executing the binary).

## Concepts Introduced
* Including `stdlib.h` for basic IO.
* The `main()` entry point.
* Using `printf()` for formatted output.
* Declaring app metadata via source annotations.

---

## The Code (e.g. `contrib/coreutils/src/hello_world.c`)

```c
// BOREDOS_APP_DESC: Hello World — a minimal CLI demo.
#include <stdlib.h>

int main(int argc, char **argv) {
    // Standard library initialization is handled automatically by crt0.asm
    
    // Print a simple string to the terminal
    printf("Hello, World from BoredOS Userland!\n");
    
    // Print some formatted data
    int favorite_number = 67;
    printf("Did you know my favorite number is %d?\n", favorite_number);
    
    // Returning from main automatically terminates the process cleanly
    return 0;
}
```

## How it Works

1.  **`#include <stdlib.h>`**: We include the SDK's standard library header which gives us access to `printf`.
2.  **`int main(...)`**: Every process begins execution here (managed transparently by `crt0.asm`).
3.  **`printf(...)`**: The SDK routes this call internally directly to the `SYS_WRITE` system call, making it available on the terminal.
4.  **`return 0`**: A successful exit code.

## Running It

If you build the project, you can open the Terminal and type:
```sh
/ # hello_world
Hello, World from BoredOS Userland!
Did you know my favorite number is 67?
/ # 
```
