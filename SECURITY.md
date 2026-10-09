# Security

## Trust boundary

Sun Keyboard Tools requires an administrator account with UAC enabled on Windows 10/11 x64. The normal user app edits profiles, opens help and folders, and sends shortcuts. It does not retain an administrator token. If started elevated, it starts a fresh process with the same sign-in's verified normal desktop token before loading profiles or creating its windows. Consequently, remapped shortcuts cannot control elevated applications or the secure desktop.

Only `sun-keyboard-capture.exe` keeps administrator privileges during remapping. It must run at its fixed Program Files location. It checks ownership and permissions of the executable, UI, and path components, rejects reparse points and multiple-link files, and holds directory handles against renaming. After starting the normal UI, it permanently removes unused token privileges, including debug and impersonation privileges. It exits after 30 seconds without a client. Pausing for longer may stop it; Resume starts it again through the protected startup task, or a UAC prompt if startup is disabled.

The helper has no profile-file parser, shortcut injector, user-selected process launcher, or user-selected file writer on its request path. It accepts one fixed-size, versioned keyboard configuration per pipe connection. Integers, offsets, lengths, usage counts, and padding are validated again at this boundary. It sends only bounded status codes, report counts, and configured usage IDs. USB device opens use fixed kernel/global device namespaces instead of per-user DOS aliases.

The message pipe rejects remote clients and is limited to the interactive logon SID, administrators, and SYSTEM. Its client grant excludes the right to create another server instance. First-instance creation and elevated server image checks reject pipe impersonation. Both peers check account and logon identity; the client uses identification-only security quality of service. The helper never impersonates clients. Only an elevated client may request helper shutdown. Incomplete handshakes and blocked output have deadlines, and disconnects cancel capture.

A program already running as the same normal user can change that user's profiles, access their input, interfere with the UI, or interrupt capture. The protocol does not treat the normal app as a trusted administrator. Configuration cannot supply paths, commands, arbitrary IOCTLs, or shortcuts to the helper. This is an elevation boundary, not isolation from malware already controlling the user's desktop.

## Installation and maintenance

The installer only modifies its fixed, administrator-protected installation locations. It validates owners and write permissions, rejects reparse points and hard links, and holds parent directories while modifying files. A suspicious pre-existing installation is rejected instead of silently repairing its permissions. Normal payload extraction is refused when elevated.

The maintenance executable is an embedded build artifact; setup does not copy its own pathname from a user-writable download directory. Quiet errors return a nonzero exit code without creating a log beside that directory. Per-user driver reminders and Start menu shortcuts are written by a normal user process. Legacy shared shortcuts are removed only if that normal account already has permission. Startup has an explicit administrator-owned ACL; the normal account can read and run the task but cannot edit it. The only startup image is the protected capture helper.

Bundled USBPcap installer and uninstaller hashes are checked at execution time, with Authenticode verification and a read lock against replacement. Changing to another USBPcap release requires reviewing and updating the pins. The uninstaller pin corresponds to the signed uninstaller emitted by the bundled 1.5.4.0 installer. Custom versions must be managed through Windows Installed apps.

Application binaries use stack checks, Control Flow Guard, ASLR, DEP, CET compatibility, and System32-only import loading. Runtime DLL search is also restricted to System32. These mitigations reduce risk; they do not establish memory safety.

## Reproducing checks

Run the normal CMake/CTest build for pure decoder, maintenance, and protocol tests. `tests/package-smoke.ps1` verifies the complete embedded payload without installing a driver; on elevated CI runners it reads the PE resources as data and verifies that elevated extraction is refused.

Optional live tests are deliberately not part of CTest:

- Run `sun-security-tests.exe --paths` elevated to check protected, writable, hard-link, and reparse-point fixtures. It creates and removes only its uniquely named test directory in Program Files.
- With the installed app active and a Type 7 connected, run `sun-windows-boundary-tests.exe RESULT.txt --idle` as a normal user. It briefly pauses remapping, checks process/pipe boundaries and malformed requests, waits for idle exit, then resumes. It does not edit mappings. Its capture-ready check requires the supplied Type 7 hardware.

## Limits and reporting

The project has not undergone an independent security audit. The application, capture helper, and installer are unsigned. USBPcap is a third-party kernel driver with its own attack surface; this release does not audit or fix that driver. Its existing Windows 11 compatibility warning still applies. A compromised administrator or kernel is outside this boundary, and denial of service by the same signed-in user is not prevented.

Do not disable Windows security features to load a blocked driver. Keep Windows updated. If you suspect a security issue, use the repository's GitHub security advisory reporting feature if available; otherwise open an issue describing the affected component without publishing exploit details or private data. Do not include keyboard captures or personal profiles in public reports.

Implementation references: [Microsoft named-pipe access rights](https://learn.microsoft.com/en-us/windows/win32/ipc/named-pipe-security-and-access-rights), [System32 import loading](https://learn.microsoft.com/en-us/cpp/build/reference/dependentloadflag), and [SendInput integrity restrictions](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-sendinput).
