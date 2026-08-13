
# pokaz

A high-performance, lightweight image viewer for Windows written in native C, hardware-accelerated via OpenGL with the help of decoders (`libspng`, `libjpeg-turbo`, `libavif`) paired with native WIC integration.

## Supported Formats
- **Standard:** PNG, JPEG, BMP, GIF, TIFF, ICO
- **Modern & Extended:** WebP, JXR, HDP, WDP, AVIF


## Usage
  ```pokaz.exe [file | directory]```

- Drag and drop a folder or image file onto the window to open it.
- Use the arrow keys (or `A` / `D`) to move between images — see full [Controls](#controls) below.

## Controls

| Key / Input                | Action                   |
|----------------------------|--------------------------|
| `→`, `Page Down`, or `D`   | Next image               |
| `←`, `Page Up`, or `A`     | Previous image           |
| `Home`                     | First image              |
| `End`                      | Last image               |
| `Delete`                   | Delete current image     |
| `O`                        | Open file location       |
| `R`                        | Rotate 90°               |
| `Z` or Mouse Wheel Up      | Zoom in                  |
| `X` or Mouse Wheel Down    | Zoom out                 |
| `0`                        | Reset zoom/pan           |
| Left-click + drag          | Pan image                |
| `Space`                    | Toggle slideshow         |
| `+`                        | Increase slideshow speed |
| `-`                        | Decrease slideshow speed |
| Drag & drop onto window    | Open a file/folder       |
| `Esc`                      | Quit                     |

## Dependencies

* **[libjpeg-turbo](https://github.com/libjpeg-turbo/libjpeg-turbo)** – JPEG decoding
* **[libspng](https://github.com/randy408/libspng)** – PNG decoding
* **[libavif](https://github.com/AOMediaCodec/libavif)** – AVIF image container parsing (requires `dav1d`)

