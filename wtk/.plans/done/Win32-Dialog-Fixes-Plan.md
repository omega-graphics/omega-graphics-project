# Win32 Dialog Fixes — FSDialog & NoteDialog

**Status:** active. Small feature (bug-fix/completion, < 300 LOC), so a single
note per AGENTS.md rather than a multi-phase breakdown.

## Problem

On Windows, `NativeAlertDialog` works but `NativeFSDialog` (file open/save) and
`NativeNoteDialog` (modal informational sheet) do nothing. This is **not** a
permission / capability issue — the notification-permission machinery in
`App-Building-Plan.md` (AUMID + Start-Menu shortcut) applies only to *toast
notifications*. File and note dialogs are plain in-process UI that Windows shows
without any install-time declaration. Alert works because
`WinAlertDialog::getResult` uses `TaskDialogIndirect`, which has no COM-apartment
or presentation dependency. The other two each fail for an ordinary code reason.

## Root causes

1. **FSDialog — COM apartment mismatch.** `target/win32/mmain.cpp` initializes
   COM on the UI thread as `CoInitializeEx(NULL, COINIT_MULTITHREADED)` (MTA).
   The Windows Common Item Dialog (`IFileOpenDialog` / `IFileSaveDialog`) must be
   shown from an STA thread; on MTA `IFileDialog::Show` fails, so
   `WinFSDialog::getResult` skips the `SUCCEEDED` branch, returns an empty path
   vector, and no window ever appears — a silent no-op. `WinFSDialog`'s ctor also
   does `(void)hr;`, swallowing any `CoCreateInstance` failure.

2. **NoteDialog — never presented + debug spam.** `WinNoteDialog`'s constructor
   only *builds* the DLGTEMPLATE; it never calls `show()`. Nothing else can —
   the `NativeNoteDialog` base interface doesn't declare `show()` and
   `AppWindow::openNoteDialog` only calls `Create()`. (macOS `CocoaNoteDialog`
   presents in its constructor via `show()`; Win32 must match that contract.)
   The ctor is also littered with ~9 blocking `MessageBoxA` step-through prompts
   left over from bring-up, and it uses `desc.title` for the body text instead of
   `desc.str`.

## Changes

### 1. UI thread → STA (`target/win32/mmain.cpp`)
Switch `CoInitializeEx(NULL, COINIT_MULTITHREADED)` to
`CoInitializeEx(NULL, COINIT_APARTMENTTHREADED)`. STA is the conventional
apartment for a GUI thread precisely because shell dialogs, OLE drag/drop, and
the clipboard require it.

### 2. Decouple the compositor frame worker from the UI thread's apartment (`src/Composition/Compositor.cpp`)
COM apartments are **per-thread**. Today the `CompositorFrameWorker` thread never
calls `CoInitializeEx`, so it runs in the process *implicit MTA* that the UI
thread's `COINIT_MULTITHREADED` established. Once the UI thread becomes STA we
make the worker's MTA membership explicit and independent: call
`CoInitializeEx(nullptr, COINIT_MULTITHREADED)` at the top of the worker thread
body and `CoUninitialize()` after its loop exits (Windows-only, `#ifdef _WIN32`
to match the local Composition convention). The worker does Direct2D /
DirectComposition / DirectWrite background rendering, which is free-threaded and
belongs in the MTA — so behavior is functionally identical to today (implicit
MTA → explicit MTA); the change only removes the dependency on what apartment the
UI thread happens to pick. This is the "separate initialize/teardown on
compositor thread startup/shutdown" the split calls for.

### 3. NoteDialog completion (`src/Native/win/WinDialog.cpp`)
- Remove all `MessageBoxA` debug prompts from `WinNoteDialog`'s constructor.
- Present on construction: call `show()` at the end of the ctor, matching
  `CocoaNoteDialog`.
- Use `desc.str` for the static body text (keep `desc.title` for the caption).
- Also stop swallowing the `CoCreateInstance` HRESULT in `WinFSDialog`'s ctor
  (trace failure to `std::cerr` so a broken create is diagnosable).

### 4. NoteDialog 64-bit crash — `lpwAlign` pointer truncation (`src/Native/win/WinDialog.cpp`)
Found after the above landed: presenting the note dialog faulted with
`c0000005`. `lpwAlign` (the DWORD-boundary aligner for the in-memory
DLGTEMPLATE) declared its scratch as `ULONG`, which is 32-bit on 64-bit Windows
(LLP64). Casting the 64-bit template pointer through `ULONG` truncated the high
half; the reconstructed pointer faulted on the first `DLGITEMTEMPLATE` write.
This helper is used *only* by the note-dialog template builder, so Alert
(`TaskDialogIndirect`) and FSDialog (`IFileDialog`) were unaffected. Fix: use
`ULONG_PTR` (pointer-sized), and add 3 (not 1) before masking so the address
rounds *up* to the DWORD boundary (the `++` form rounded down for two of the four
alignments — latent, but masked by the crash). `UniString` needs no expansion:
it is UTF-16-backed and `fromUTF8`/`getBuffer` already give Win32 exactly the
UTF-16 the template format requires.

### 5. NoteDialog — replace hand-rolled DLGTEMPLATE with TaskDialogIndirect (`src/Native/win/WinDialog.cpp`)
After the `lpwAlign` fix, the note dialog stopped crashing but did not display,
and then a `DefDlgProc` recursion (fix #4-adjacent) surfaced a `c00000fd` stack
overflow: the `DLGPROC` default case called `DefDlgProcA`, which is the dialog
class's own window procedure and re-enters the `DLGPROC` infinitely. Fixing that
left the dialog still not showing. Three successive faults (AV, stack overflow,
silent no-show) from ~110 lines of manual in-memory `DLGTEMPLATE` byte-packing
is a wrong-tool signal: the descriptor is only `title` + `str` with one OK
button, which `TaskDialogIndirect` renders directly — the very API the working
`WinAlertDialog` already uses (comctl32 v6, linked via the existing
`#pragma comment(lib, "comctl32.lib")`). Replaced the entire `WinNoteDialog`
(class, `lpwAlign`, `DlgProc`, template ctor, `show`, `close`, `hgbl`) with a
single `TaskDialogIndirect` call presented on construction (modal, single OK,
info icon), reusing the file's `widen()` and `parentHwnd()` helpers and logging
any non-success `HRESULT`. Eliminates the whole class of template bugs.

## Verification
Windows build is handed to the user (WSL constraint). After it builds, verify
visually via user screenshot (AGENTS.md Visual Debugging): (a) File → Open shows
the system file picker and a chosen path resolves; (b) About shows the note
dialog with the descriptor's body text and no stray debug MessageBoxes;
(c) existing Alert dialog still works; (d) the app still renders frames (worker
thread MTA init did not disturb the compositor).
