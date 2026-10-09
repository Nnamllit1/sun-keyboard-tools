# Architecture and verification boundaries

The original C++20 application lives in user space. The independently maintained
USBPcap USB class filter driver supplies reports that the normal Windows keyboard
stack does not expose for the tested Type 7. Windows Raw Input and key hooks do
not recover keys already discarded below them. Administrator access did not
permit direct HID reads in the feasibility tests. Built-in USB ETW recorded
transfer metadata without the needed report bytes on that system.

`src/backend.cpp` discovers `USBPcap*` DOS devices, obtains each root hub's
symbolic link, walks hub ports, and matches the selected profile's VID/PID.
Only the matched USB device address is enabled in USBPcap's kernel capture
filter. Neither the capture-all flag nor auto-capture-new-device bit is used.
The backend checks the device identity and address on the parent hub port
before processing a read and while waiting for input. Disconnect causes capture
to close and enumeration to restart. One matching keyboard is supported.

The driver emits classic pcap. `src/core.cpp` handles fragmented and coalesced
records with bounded storage, validates headers, and ignores truncated records.
Only successful completed interrupt-IN transfers with the selected address,
endpoint, and exact report length reach the array decoder. Only mapped new
presses reach `SendInput`. Ordinary reports remain in memory; no packet capture,
typed content, telemetry, or clipboard content is written to disk or sent over
the network. The kernel filter itself attaches to the USB class system-wide.

`src/app.cpp` provides a Win32 tray icon, key monitor, editable shortcuts, saved
profile selection, and startup control. Original defaults are copied to
LocalAppData only if missing. Saving uses a temporary file plus Windows atomic
replacement. Capture runs on a cancellable worker thread; the UI polls status.

`src/windows.cpp` registers a per-user Task Scheduler logon task with an
interactive token and highest available privilege. No password is stored. It
does not run a remapping service before sign-in, where desktop input injection
would be inappropriate. Setup is intended for administrator accounts. Elevating
as a different account also changes the account owning profiles and startup.

`src/setup.cpp` embeds the app, default profile, help, notices, and the unmodified
USBPcap installer together with its exact corresponding source archive. CMake
pins SHA-256 hashes. Packaging verifies Authenticode, and setup verifies the
extracted driver installer's signature before invoking its `/S` option. Existing
USBPcap installations are reused, never silently downgraded or uninstalled.
Setup writes to Program Files and rejects reparse paths. Its maintenance screen
supports app repair, driver setup, and removal. Repair preserves saved profiles
and the invoking user's startup preference. Uninstall removes known app files
and that user's task; saved profiles remain. Shared USBPcap is kept unless the
user explicitly selects its removal. Driver removal uses the signed upstream
uninstaller and waits for it to finish.

Driver reinstall spans a reboot boundary. Setup removes USBPcap and registers
a per-user RunOnce entry that reopens the graphical driver screen at sign-in.
The user selects Install driver, then restarts again to activate it. Pending
driver removal, loaded capture devices, and incomplete installer registration
prevent a conflicting install. Setup never reboots or changes driver security.

A running app uninstaller moves to a unique protected cleanup path before its
files are removed. Only that unique path is scheduled for later cleanup; an
immediate app reinstall cannot be deleted by the previous uninstall's cleanup.

The original application and setup executable are currently unsigned. USBPcap's
own installer and supplied driver carry upstream signatures. A driver signature
is not a guarantee of compatibility with every Windows build. USBPcap 1.5.4.0 has
an [unresolved Windows 11 crash report](https://github.com/desowin/usbpcap/issues/146).

## Validation

Automated tests cover malformed USB/pcap data, bytewise and split reads,
coalesced records, wrong-device/endpoint rejection, shortcut parsing, rollover,
duplicate usages, and press/release behavior. Installer extraction can be tested
without installation using `SunKeyboardTools-Setup.exe --extract NEW_DIRECTORY`.
Verify extracted hashes against `third-party/NOTICE.txt` and the built app.

Those checks do not prove physical key delivery, driver compatibility, boot
startup, or all application shortcuts. Test those on an actual keyboard after
USBPcap is active. On first driver installation, a reboot is normally required.
Do not describe an untested model as supported.

The Type 7's Copy, Paste, and Undo actions and actual logon startup have now
passed a hardware check on Windows 11 build 26300. See the [validation record](VALIDATION.md)
for exactly what was checked and what remains untested.

Primary references: [USBPcap capture format](https://desowin.org/usbpcap/captureformat.html),
[upstream release](https://github.com/desowin/usbpcap/releases/tag/1.5.4.0),
[SendInput](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-sendinput).
