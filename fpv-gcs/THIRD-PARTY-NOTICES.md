# Third-party components

FPV GCS ships shared libraries and plugins from the following projects. Their
copyright notices and license texts are included in the `licenses` directory.

| Component | Upstream / corresponding source | License information |
| --- | --- | --- |
| Qt 6.11.1 (Core, Gui, Widgets, Network, Svg) | https://download.qt.io/archive/qt/6.11/6.11.1/single/ | LGPL-3.0 and other licenses; see `licenses/qtbase` and `licenses/qtsvg` |
| SDL 3.4.20 | https://github.com/libsdl-org/SDL/releases/tag/release-3.4.20 | zlib; see `licenses/SDL3.txt` |
| GStreamer and its runtime dependencies | https://gstreamer.freedesktop.org/src/ | Component-specific notices in `licenses/gstreamer` |
| GCC / MinGW runtime | https://gcc.gnu.org/ and https://www.mingw-w64.org/ | GCC runtime library exception and component-specific licenses |

Libraries are dynamically linked and remain separate files. Installation does
not prohibit replacing compatible shared libraries or reverse engineering for
debugging modifications to LGPL-covered libraries. Source and build instructions
for the application are available in this repository. This notice does not grant
a new license to the application's own source code or artwork.
