# Validation

## Physical hardware

The 0.1.0 remapping implementation was checked on 2026-10-09 with a Sun Type 7 USB keyboard (`0430:00a2`), Windows 11
Pro build 26300, and the bundled USBPcap 1.5.4.0 driver.

| Check | Evidence |
| --- | --- |
| Driver installation | Setup returned 3010; driver and catalog signatures validated; USB filter and service registered. |
| Activation after restart | USBPcap service running with exit code 0 after the Windows restart. |
| Automatic startup | The installed app's per-user logon task launched it after sign-in. |
| Device discovery | The running backend connected to the matching Type 7 and reported Ready. |
| Copy, Paste, Undo | The keyboard owner confirmed all three physical keys performed the expected actions in a text editor. The app recorded 3 extra-key presses, 3 shortcuts sent, and 0 skipped actions. |

The remaining default keys, unplug/replug recovery, multiple user sessions,
other Sun models, and long-term driver stability have not been physically
verified. This is evidence for the tested setup, not a Windows-wide compatibility
guarantee. The upstream Windows 11 USBPcap crash report remains unresolved.

## Software and packaging

- Release builds pass with Visual Studio 2022, MSVC 14.44, and Windows SDK 26100.
- The core test executable passes 198 checks covering chord/profile validation,
  fragmented and coalesced pcap streams, invalid USB headers, device/endpoint
  filtering, held-key transitions, duplicates, rollover, and profile round trips.
- Live settings checks confirm shortcut persistence, pause/resume, and creation
  and removal of the highest-privilege per-user logon task.
- The settings window was visually inspected.
- Installer extraction was checked against the built app and pinned upstream
  release hashes. The driver's corresponding source and notices are bundled.

## Graphical maintenance in 0.2.0

- The original 198 core checks pass, along with 10 driver-state decision cases.
- Package checks verify app/help/profile contents, executable version metadata,
  exact USBPcap release hashes, and prompt-free failure on an invalid extraction.
- An actual upgrade, installed-uninstaller removal, and immediate app reinstall
  succeeded. Saved profile hashes and the running USBPcap driver were unchanged.
- Cleanup was checked to target only the unique old uninstaller path, never the
  reinstalled app's normal path.
- The graphical maintenance, driver-options, and uninstall screens were inspected.
  Graphical app repair reached its completion window; cancellation was checked.
- Driver removal/reinstallation across two restarts has not been exercised on
  the working keyboard system. Its state decisions are tested; the signed
  upstream driver installer/uninstaller perform the actual driver operations.

The original application code has no selected license. Third-party license
terms remain in force; see [COPYRIGHT.txt](../COPYRIGHT.txt) and
[third-party notices](../third-party/NOTICE.txt).
