<div align="center">
  <h1>Example Applications</h1>
  <p><em>From basic terminal output to direct Framebuffer graphics and Network applications.</em></p>
</div>

---

Welcome to the examples directory! These guides are designed to help you understand how to write C applications for the BoredOS userland, utilizing the custom `libc` SDK. 

The examples are listed in order of increasing complexity. Click on a tutorial to view the complete source code and an explanation of the concepts it introduces.

## 🟢 Beginner

*   **[`01_hello_cli.md`](01_hello_cli.md)**: The absolute basics. Learn how to write a simple Terminal program that outputs text and processes standard command-line arguments.

## 🟡 Intermediate

*   **[`02_fb_gradient.md`](02_fb_gradient.md)**: Dive into low-level graphics. Learn how to open the raw `/dev/fb0` framebuffer device, query display parameters, use `ioctl` to disable TTY text blitting (`KD_GRAPHICS`), perform double-buffering, and cleanly restore text console mode (`KD_TEXT`) on exit.
*   **[`03_fb_bouncing_ball.md`](03_fb_bouncing_ball.md)**: Real-time high-performance animations and physics. Learn how to map display memory directly using `mmap()`, draw solid shapes using screen stride (pitch) equations, calculate elastic boundary collisions, and intercept termination signals for secure console recovery.

## 🔴 Advanced

*   **[`04_tcp_client.md`](04_tcp_client.md)**: Outbound networking. This example demonstrates how to verify the network state, perform DNS name resolution, establish a TCP connection over port 80, transmit a standard HTTP/1.1 request, and read/dump the raw response to the console.

## 🔵 Graphical / GUI (Nova Toolkit)

The native desktop environment source directory contains structured GUI apps using the Nova Toolkit:
*   **[helloworld.c](file:///Users/chris/BoredOS/contrib/nova/src/helloworld.c)**: Demonstrates building tabbed windows, buttons, checkboxes, radio groups, sliders, combo boxes, list boxes, tables, text areas, input fields, modal dialogs, and a custom linear gradient canvas.
*   **[about.c](file:///Users/chris/BoredOS/contrib/nova/src/about.c)**: A clean implementation of a system specification window featuring branding images, centered label text, and non-resizable layouts.

---

> [!TIP]
> If you want to test these out, simply create a new `.c` file in an external repository source directory such as `contrib/coreutils/src/` for terminal apps or `contrib/netutils/src/` for network utilities, paste the example code, then run `make clean && make run` from the project root!

---
