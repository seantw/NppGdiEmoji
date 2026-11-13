# GDI Emoji for Notepad++

GDI Emoji displays color emoji and monochrome symbols in UTF-8 documents in Notepad++'s GDI mode. It draws each symbol within a square about as wide as a CJK character, subject to the line height and available space. The plugin leaves the document text intact.

## Install

GDI Emoji supports Notepad++ x64 on Windows 10 and 11. Look for **GDI Emoji** in **Plugins → Plugins Admin**. To install manually, download the x64 ZIP from [Releases](https://github.com/seantw/NppGdiEmoji/releases), close Notepad++, and extract `NppGdiEmoji.dll` to `plugins\NppGdiEmoji\` under your Notepad++ folder. Then restart Notepad++.

In **Settings → Preferences → MISC.**, select **GDI (most compatible)**. Open a UTF-8 document to see the plugin in action.

![GDI rendering mode in Notepad++ preferences](screenshot/setting.png)

## Try it

Paste these lines to compare text and emoji presentation, check spacing, and try selection:

```text
中✈中　中✈️中　中❤中　中❤️中
中😀中　中🇻🇳中　中👍🏽中　中1️⃣中
中👩‍💻中　中👩🏽‍💻中　中🏳️‍🌈中
```

- Color emoji use **Segoe UI Emoji**. Supported text-style symbols such as `✈`, `❤`, and `✌` use **Segoe UI Symbol** and follow the editor's text color.
- Supported sequences include emoji presentation variants, country flags, subdivision flags for England, Scotland, and Wales, skin tones, keycaps, and joined emoji of up to eight elements.
- With a single caret, the Left and Right keys skip over supported sequences. Shift navigation and mouse selection snap to sequence boundaries. Drag selections snap when you release the mouse button.

## Display limitations

- The installed Windows fonts determine which glyphs are available and how they look.
- With word wrap enabled, the plugin keeps Scintilla's original spacing. It can redraw a sequence on one visual line when the background allows it. Sequences split across visual lines use the editor's own rendering.
- With word wrap disabled, U+FE0F reserves about half a CJK character's width. In documents up to 256 KiB, the plugin can reduce excess spacing for certain sequences if the affected characters occur only in eligible sequences and the editor font supports the required zero-width placeholder.
- The plugin scans up to the first 256 KiB of each visible document line.

## Build

Add x64 MinGW-w64 (`g++.exe` and `windres.exe`) to `PATH`, then run the batch file from the project root. The script uses PowerShell, `tar.exe`, and `certutil.exe` to package the build and print the ZIP's SHA-256.

```bat
.\build-x64.bat
```

Source files are in `src\`. The build writes `build\NppGdiEmoji.dll` and packages it as `release\NppGdiEmoji-<version>-x64.zip`. Git ignores both output directories.

## License

NppGdiEmoji is released under the **MIT License**. See [LICENSE](LICENSE).
