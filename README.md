# USB Keyboard To Serial

The USB Keyboard to Serial bridge enables ESP32S3 devices to function as a USB host for standard HID keyboards, translating key presses into ASCII characters and transmitting them via UART. This allows keyboard input to be routed to serial devices such as FPGAs, microcontrollers, or other systems that communicate over serial protocols.

# Keycode Translation

The translation system uses two static lookup tables to map keycodes to ASCII values.

| ASCII                      | USB Key Code | USB Modifier |
|----------------------------|--------------|--------------|
| `a` ~ `z`                  | 0x04 ~ 0x1D  |              |
| `A` ~ `Z`                  | 0x04 ~ 0x1D  | SHIFT        |
| `1` ~ `9`, `0`             | 0x1E ~ 0x27  |              |
| <code>!@#$%^&*()</code>    | 0x1E ~ 0x27  | SHIFT        |
| Enter                      | 0x28         |              |
| Esc                        | 0x29         |              |
| Backspace                  | 0x2A         |              |
| Tab                        | 0x2B         |              |
| Space                      | 0x2C         |              |
| <code>-=[]\ ;'`,./</code>  | 0x2D ~ 0x38  |              |
| <code>_+{}\| :"~<>?</code> | 0x2D ~ 0x38  | SHIFT        |
