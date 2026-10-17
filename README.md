# tuxpad

A Notepad++-style source code editor for Linux, built with GTK+ 3 and
GtkSourceView.

## Features

- **Tabbed editing** — multiple documents, reorderable tabs, close buttons, and
  a `*` modified indicator
- **Syntax highlighting** with automatic language detection and a **Language**
  menu covering every GtkSourceView language
- **Line numbers**, current-line highlight, bracket matching, auto-indent
- **Find / Replace** with match case, whole word, **regular expressions**, and
  wrap around; Find Next/Previous; **Go to Line**
- **Find in Files** (Ctrl+Shift+F) — search a directory and its subdirectories
  with the same criteria (match case, whole word, regex, file filters); results
  appear in a dockable panel, double-click a hit to jump to it
- **Plugins** — drop a `.so` into a plugin directory to add Plugins-menu
  commands (see below); a `sample_tools` plugin ships as an example
- **Tools** menu:
  - **Format Document** (Ctrl+Shift+I) — reformats via `clang-format`,
    `astyle`, or `indent` if installed
  - **Syntax Check** (F7) — compiles the C/C++ buffer with `gcc`/`g++
    -fsyntax-only` and lists errors/warnings in the results panel;
    double-click a message to jump to the line
  - **Flow Diagram** (F8) — draws the call graph of the C/C++ functions in a
    window, rendered built-in with Cairo (no external tools needed). Each
    function box shows its **signature** (return type, name, parameters) in the
    header and, below, its **parameters**, detected **local variables**, and
    **return values**; arrows show which function calls which.
    - **Drag** a box to rearrange it; **click** a box to jump to that function
      in the editor
    - **Flow Diagram (Directory)** (Shift+F8) builds a combined call graph of
      every C/C++ file in a chosen directory (cross-file calls included)
    - Save the diagram as PNG
- **View**: word wrap, zoom in/out/reset, line-number toggle, color themes
- Notepad++-style **status bar**: length/lines, Ln/Col/Sel, EOL, encoding,
  language
- **Drag & drop** files to open; open files from the command line
- **Desktop integration**: app-menu entry, icon, and "Open With" support
- Printing with line numbers

## Keyboard shortcuts

| Action        | Shortcut            | Action         | Shortcut      |
|---------------|---------------------|----------------|---------------|
| New           | Ctrl+N              | Find           | Ctrl+F        |
| Open          | Ctrl+O              | Find Next      | F3            |
| Save          | Ctrl+S              | Find Previous  | Shift+F3      |
| Save As       | Ctrl+Shift+S        | Replace        | Ctrl+H        |
| Save All      | Ctrl+Alt+S          | Find in Files  | Ctrl+Shift+F  |
| Format Doc    | Ctrl+Shift+I        | Go to Line     | Ctrl+G        |
| Syntax Check  | F7                  | Flow Diagram   | F8            |
| Close tab     | Ctrl+W              | Zoom In        | Ctrl++        |
| Print         | Ctrl+P              | Zoom Out       | Ctrl+-        |
| Undo / Redo   | Ctrl+Z / Ctrl+Y     | Reset Zoom     | Ctrl+0        |
| Select All    | Ctrl+A              | Exit           | Ctrl+Q        |

## Build

Install the dependencies (Debian/Ubuntu):

```sh
sudo apt install build-essential libgtk-3-dev libgtksourceview-3.0-dev
```

Optional, for the Tools menu:

```sh
sudo apt install clang-format    # or: astyle / indent — for Format Document
```

(`gcc`/`g++` for Syntax Check come with `build-essential`; the Flow Diagram
needs nothing extra.)

Then:

```sh
make
make run          # or ./tuxpad
```

## Install

Per-user (no root; installs into `~/.local`, adds the app-menu entry/icon):

```sh
make install-user
```

System-wide (all users):

```sh
sudo make install        # -> /usr/local/bin/tuxpad
```

Either target installs the binary, the compatibility schema, the bundled
plugins, a `.desktop` launcher, and the icon, then refreshes the desktop and
icon caches — so tuxpad shows up in your application menu and in the
"Open With" list for text files. `make install-user` also drops a launcher
icon on your desktop. Uninstall with `make uninstall-user` or
`sudo make uninstall`.

### Desktop icon

`make install-user` places a `tuxpad.desktop` launcher in your desktop folder
(it honours localised names such as `~/Schreibtisch`). To (re)create just the
desktop icon:

```sh
make desktop-icon PREFIX=$HOME/.local
```

On GNOME, the first time you may need to right-click the icon and choose
**"Allow Launching"** ("Starten erlauben") to trust it.

## Usage

```sh
tuxpad                 # start with an empty document
tuxpad file1.c file2.h # open files (non-existent paths create new named tabs)
```

## Plugins

A plugin is a shared object that exports `int tuxpad_plugin_register(TuxpadApi *)`.
During registration it adds items to the Plugins menu; the callbacks can read
and modify the active document through the `TuxpadApi` function table (see
[`include/tuxpad-plugin.h`](include/tuxpad-plugin.h)).

Plugins are loaded at startup from:

- `plugins/` next to the binary (the build tree)
- `<prefix>/lib/tuxpad/plugins/` (after install)
- `~/.config/tuxpad/plugins/`

Build a plugin:

```sh
gcc -shared -fPIC -Iinclude plugins/myplugin.c -o plugins/myplugin.so \
    $(pkg-config --cflags --libs gtk+-3.0)
```

The included [`plugins/sample_tools.c`](plugins/sample_tools.c) adds
UPPERCASE/lowercase conversion, Insert Date & Time, and a Word Count dialog.

## Troubleshooting

### "Settings schema … does not contain a key named 'antialiasing'"

On GNOME 47+ (e.g. GNOME 50) the font keys were moved out of the
`org.gnome.settings-daemon.plugins.xsettings` schema, but GTK 3.24 still reads
them there and aborts at startup when no settings daemon is feeding the values.

tuxpad handles this automatically: it ships a corrected copy of that schema in
the `schemas/` directory next to the binary (installed to
`<prefix>/share/tuxpad/schemas`), detects the missing key at startup, and
re-execs itself with `GSETTINGS_SCHEMA_DIR` pointing at the fixed copy. No
system files are modified.

If you ever need to do it manually:

```sh
GSETTINGS_SCHEMA_DIR="$PWD/schemas" ./tuxpad
```

### Running from a snap-confined shell (e.g. VS Code snap terminal)

A snap terminal injects `GTK_PATH` / `GDK_PIXBUF_MODULE_FILE` pointing at snap
libraries, which breaks the image loader. Launch from a normal terminal, or
strip those variables:

```sh
env -u GTK_PATH -u GDK_PIXBUF_MODULE_FILE ./tuxpad
```
