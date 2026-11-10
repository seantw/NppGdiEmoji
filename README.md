# GDI Emoji for Notepad++

GDI Emoji draws color emoji in UTF-8 documents in Notepad++'s GDI mode. The emoji drawing area targets the measured width of a CJK character in the default editor style, capped by the line height. Scintilla reserves a matching or slightly wider space and handles text layout, caret movement, and selection. Document bytes stay intact.

## Install and try it

Close Notepad++ x64, copy `NppGdiEmoji.dll` to `plugins\NppGdiEmoji\` under your Notepad++ folder, and restart Notepad++.

In **Settings → Preferences → MISC.**, select **GDI (most compatible)**. Open a UTF-8 document to see supported emoji automatically.

![GDI rendering mode in Notepad++ preferences](screenshot/setting.png)

Paste these lines to check alignment, caret movement, and scrolling:

```text
A😀B A🚀B A👍B A✅B
A★B A♥B A☺B A❤️B
ABC 中文 123
```

The first line uses supported single-code-point emoji. The second line contains text-default symbols and an emoji variation sequence, which this build leaves to Notepad++.

## Preview limitations

- This build targets Windows 10/11 and Notepad++ x64 in GDI mode with UTF-8 documents. Color glyph coverage depends on Windows' Segoe UI Emoji font.
- The overlay supports characters with Unicode's `Emoji_Presentation` property, excluding `Emoji_Component` characters such as regional indicators and skin-tone modifiers. A flag, skin-tone sequence, keycap, ZWJ sequence, or variation sequence is not laid out and painted as a single emoji. A supported base character inside a sequence can still be drawn individually.
- The overlay scans up to the first 256 KiB of each visible document line.

## Build

Add x64 MinGW-w64 (`g++.exe` and `windres.exe`) to `PATH`, then run the batch file from the project root. Packaging uses Windows' built-in PowerShell, `tar.exe`, and `certutil.exe`. The build has been checked with Strawberry Perl's toolchain.

```bat
.\build-x64.bat
```

Source files are in `src\`. The script creates `build\NppGdiEmoji.dll` and `release\NppGdiEmoji-0.1.0-x64.zip`, then prints the ZIP's SHA-256. Both output directories are ignored by Git. The DLL includes a Windows version resource.

## License

NppGdiEmoji is released under the **MIT License**. See [LICENSE](LICENSE).
