
# pokaz

A high-performance, lightweight image viewer for Windows written in native C, hardware-accelerated via OpenGL with the help of decoders (`libspng`, `libjpeg-turbo`, `libavif`) paired with native WIC integration.

## Supported Formats
- **Standard:** PNG, JPEG, BMP, GIF, TIFF, ICO
- **Modern & Extended:** WebP, JXR, HDP, WDP, AVIF, DDS


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


### 1. Build `libavif`
```bat
cd ext\dav1d
meson setup build --default-library=static --buildtype=release -Db_vscrt=mt
ninja -C build
```
```bat
cmake -S . -B build -G Ninja -DBUILD_SHARED_LIBS=OFF -DAVIF_CODEC_DAV1D=LOCAL -DAVIF_LIBYUV=OFF -DAVIF_BUILD_APPS=OFF -DCMAKE_C_FLAGS_RELEASE="/MT /O2 /Ob2 /DNDEBUG" -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

