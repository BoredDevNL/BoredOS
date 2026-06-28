# UTF-8 Library — Application Development Guide

## Overview

The userland libc provides a lightweight UTF-8 utility module located in:

- usr/libc/src/utf-8.c
- usr/libc/include/utf-8.h

This module is designed for **direct use in applications** requiring UTF-8 handling. It provides basic primitives for decoding, encoding, and traversing UTF-8 strings safely.

It is intended for:

- text rendering
- terminal input/output
- cursor movement
- string processing at the character level

---

## Synopsis

```c
#include "utf-8.h"

uint32_t text_decode_utf8(const char *s, int *advance);
int text_encode_utf8(uint32_t cp, char *out);

const char* text_next_utf8(const char *s);
const char* text_prev_utf8(const char *start, const char *s);

int text_strlen_utf8(const char *s);
```

---

## API Reference

### text_decode_utf8

```c
uint32_t text_decode_utf8(const char *s, int *advance);
```

Decodes a UTF-8 sequence into a Unicode code point.

- `s`: pointer to current position in a UTF-8 string
- `advance`: receives number of bytes consumed

Returns:

- decoded Unicode code point (`uint32_t`)
- `0` if input is null or empty
- `0xFFFD` for invalid sequences

---

### text_encode_utf8

```c
int text_encode_utf8(uint32_t cp, char *out);
```

Encodes a Unicode code point into UTF-8.

- `cp`: Unicode code point
- `out`: buffer receiving encoded bytes

Returns:

- number of bytes written (1–4)
- writes replacement character if `cp` is invalid

---

### text_next_utf8

```c
const char* text_next_utf8(const char *s);
```

Advances to the next UTF-8 character.

Returns a pointer to the next character boundary.

---

### text_prev_utf8

```c
const char* text_prev_utf8(const char *start, const char *s);
```

Moves backward to the previous UTF-8 character.

- `start`: beginning of the buffer
- `s`: current position

Used for reverse traversal and cursor movement.

---

### text_strlen_utf8

```c
int text_strlen_utf8(const char *s);
```

Counts UTF-8 characters (code points), not bytes.

---

## Usage Examples

### Iterating over UTF-8 characters

```c
const char *p = text;

while (*p) {
    int adv;
    uint32_t cp = text_decode_utf8(p, &adv);

    /* process cp */

    p += adv;
}
```

---

### Cursor movement

```c
cursor = text_next_utf8(cursor);
cursor = text_prev_utf8(buffer_start, cursor);
```

---

### Encoding a character

```c
char out[4];
int len = text_encode_utf8(0x20AC, out);
```

---

### Backspace handling

```c
char *prev = (char*)text_prev_utf8(buffer, cursor);
cursor = prev;
```

---

## Implementation Notes

### UTF-8 Encoding

The implementation supports:

- 1 byte: `0x00 – 0x7F`
- 2 bytes: `0x80 – 0x7FF`
- 3 bytes: `0x800 – 0xFFFF`
- 4 bytes: `0x10000 – 0x10FFFF`

---

### Replacement Character

Invalid sequences are replaced with:

- code point: `0xFFFD`
- UTF-8 encoding: `0xEF 0xBF 0xBD`

---
### UTF-8 Byte Structure

The following diagram illustrates how UTF-8 bytes are structured, including
ASCII, continuation bytes, and multi-byte sequence headers:

<img width="815" height="1003" alt="image" src="https://github.com/user-attachments/assets/0d289a94-6037-4039-87a3-125c0c0e83d0" />
<sub>Source: <a href="https://www.youtube.com/watch?v=vpSkBV5vydg">Nic Barker — "UTF-8, Explained Simply"</a> (YouTube)</sub>

---

### Control Signals

Some decoded code points correspond to control signals instead of printable characters.

ASCII control range:

- `0x00 – 0x1F`

Examples:

- `0x08` → Backspace
- `0x09` → Tab
- `0x0A` → Line Feed
- `0x0D` → Carriage Return
- `0x1B` → Escape

These are typically interpreted by:

- terminal logic
- shell input handling
- system interfaces

---

### Non-ASCII Characters

Characters outside the ASCII range (`0x00 – 0x7F`) are encoded using multi-byte UTF-8 sequences.

Examples:

- 'é' → `0xC3 0xA9`
- '€' → `0xE2 0x82 0xAC`

Decoded values:

- 'é' → `U+00E9`
- '€' → `U+20AC`

---

### Modifiers and Layout

Character output depends on:

- keyboard layout
- modifier keys (Shift, Ctrl, AltGr)

Example:

- `KEY_E` → 'e'
- `KEY_E + SHIFT` → 'E'
- `KEY_E + AltGr` → '€'

---

## Also worth watching

If you want to dive deeper or simply get a better intuitive understanding of UTF-8, the video below is highly recommended:

[Nic Barker — "UTF-8, Explained Simply"](https://www.youtube.com/watch?v=vpSkBV5vydg)


