# Ved — Visual EDitor

**Ved** is a fast, dependency-light terminal IDE written in C (libc + libutil). Inspired by modern code editors like Visual Studio Code, Ved brings rich GUI-like IDE workflows — including multi-tab editing, file exploration, real PTY terminal emulator, Language Server Protocol (LSP) diagnostics, mouse drag-resizable panes, and a command palette — directly into your terminal.

---

## ✨ Features

- 🔌 **Language Server Protocol (LSP) Integration**
  - Out-of-the-box JSON-RPC 2.0 LSP support for **C/C++** (`clangd`), **Python** (`pylsp`), **TypeScript/JavaScript** (`typescript-language-server`), **Shell** (`bash-language-server`), and **Rust** (`rust-analyzer`).
  - Real-time diagnostic error (`>`) and warning (`!`) markers in the gutter margin.
  - Dedicated **Problems Panel** (`Ctrl+L`) listing all file diagnostics with line/column coordinates.
  - Status bar indicator showing LSP server state (`clangd OK`, `x3 !1`, `[no LSP]`).

- 📐 **Resizable Panes (Mouse Dragging)**
  - Drag vertical and horizontal pane borders with the mouse to dynamically resize the **File Explorer**, **Terminal Pane**, and **Diagnostics Panel**.
  - Highlighted drag handles indicate active interactive borders.

- 📁 **File Explorer Pane** (`Ctrl+E`)
  - Tree-view file explorer supporting directory expansion and interactive file opening.
  - Folder and file status indicators.

- 🖥️ **Integrated ANSI Terminal Pane** (`Ctrl+T`)
  - Full-featured embedded terminal running a real sub-shell (PTY) inside the IDE layout.
  - Seamless mouse and key focus navigation between editor and terminal panes (`Ctrl+\`).

- 🎯 **Command Palette** (`Ctrl+P`)
  - Fuzzy-searchable command menu for quick access to all editor commands, toggles, and functions.

- 🎨 **Syntax Highlighting & Modern Themes**
  - Built-in syntax highlighting for C/C++, Python, JavaScript, TypeScript, Rust, Shell, HTML/CSS, JSON, and Markdown.
  - Modern Dark (VS Code Dark+ inspired) and Light themes (`Ctrl+K Ctrl+T`).

- ⚡ **Rich Text Editing**
  - Auto-pairing of brackets (`()`, `[]`, `{}`) and quotes (`""`, `''`).
  - Multi-tab management (`Ctrl+N`, `Ctrl+W`, `Ctrl+Left/Right`).
  - Unlimited Undo / Redo (`Ctrl+Z`, `Ctrl+Y`).
  - Line manipulation: Duplicate (`Ctrl+D`), Delete (`Ctrl+K`), Toggle Comment (`Ctrl+/`).
  - Search & Replace (`Ctrl+F`, `Ctrl+H`), Go-to-Line (`Ctrl+G`).

---

## 🚀 Building & Running

### Requirements
- C compiler (`gcc` or `clang`)
- POSIX system (Linux / macOS) with `libutil` (standard POSIX utility library for PTY creation)

### Compilation

```bash
gcc -O2 -Wall -Wextra -o ved ved.c lsp.c -lutil
```

### Running

```bash
# Launch in current directory
./ved

# Open a specific file
./ved main.c

# Open a project directory
./ved /path/to/project
```

---

## ⌨️ Keyboard Shortcuts & Controls

### Global Controls

| Key | Action |
|-----|--------|
| `Ctrl+P` | Open Command Palette |
| `Ctrl+E` | Toggle File Explorer |
| `Ctrl+T` | Toggle Integrated Terminal |
| `Ctrl+L` | Toggle Problems / Diagnostics Panel |
| `Ctrl+\` | Cycle Focus between Panes (Editor / Explorer / Terminal) |
| `Ctrl+Q` | Quit Ved (prompts if unsaved changes exist) |

### Navigation & Tabs

| Key | Action |
|-----|--------|
| `Ctrl+N` | New File |
| `Ctrl+W` | Close Current Tab |
| `Ctrl+Left` / `Ctrl+Right` | Switch to Previous / Next Tab |
| `Ctrl+G` | Jump to Line Number |
| `Page Up` / `Page Down` | Page Up / Down in Active Buffer |

### Editing

| Key | Action |
|-----|--------|
| `Ctrl+S` | Save Active File |
| `Ctrl+Z` | Undo |
| `Ctrl+Y` | Redo |
| `Ctrl+F` | Find in Buffer |
| `Ctrl+H` | Find & Replace |
| `Ctrl+D` | Duplicate Line |
| `Ctrl+K` | Delete Line |
| `Ctrl+/` | Toggle Line Comment (`//` or `#`) |

### Mouse Controls

- **Click File in Explorer**: Open or expand folder.
- **Click Tab**: Switch active tab.
- **Click Editor / Terminal**: Set focus.
- **Mouse Drag Borders**:
  - Drag vertical border between Explorer and Editor to adjust Explorer width.
  - Drag horizontal border above Terminal to adjust Terminal height.
  - Drag horizontal header of Problems panel to adjust Diagnostics panel height.
- **Mouse Wheel**: Scroll buffer content, Explorer list, or Problems list.

---

## 🛠️ Supported Language Servers (LSP)

Ved automatically detects installed LSP servers on your system based on file extension:

| Language | Default Binary / Command |
|----------|--------------------------|
| **C / C++** | `clangd` |
| **Python** | `pylsp` |
| **JavaScript / TypeScript** | `typescript-language-server --stdio` |
| **Shell Script** | `bash-language-server start` |
| **Rust** | `rust-analyzer` |

*(If no server is installed for a language, Ved displays `[no LSP]` in the status bar and operates smoothly as a standalone editor).*

---

## 🏗️ Architecture Overview

- **`ved.c`**: Single-file core codebase handling TUI rendering (ANSI escape sequences), buffer snapshots for undo/redo, syntax tokenization, window/pane layout calculations, PTY terminal emulator interface, mouse event parsing, and event loop `select()`.
- **`lsp.c` / `lsp.h`**: Minimal JSON-RPC 2.0 client communicating over standard POSIX subprocess pipes (`fork` + `execvp`). Parses `textDocument/publishDiagnostics` asynchronously and updates per-row error/warning flags without blocking the main render loop.

---

## 📜 License

Ved is open-source under the MIT License.
