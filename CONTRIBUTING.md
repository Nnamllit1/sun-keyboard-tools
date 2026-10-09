# Working on Sun Keyboard Tools

Start with the [build instructions](README.md#build-developers) and
[architecture](docs/ARCHITECTURE.md). To add a keyboard model, follow the
[profile guide](docs/PROFILES.md); include actual USB identity/report evidence
and say which keys were physically tested.

Run the core and maintenance tests for changes to report parsing, shortcut
handling, or driver-state decisions. For installer changes, build the package and
run `tests/package-smoke.ps1`. Do not install or remove a driver merely to run CI.
App/driver lifecycle testing requires an appropriate Windows test system.

Keep ordinary keystrokes and raw USB captures out of issue reports, fixtures, and
logs. Use small synthetic reports for tests. Never put credentials, local capture
files, generated builds, or personal configuration in a pull request.

Changes to driver dependencies must retain the corresponding source archive,
notices, pinned hashes, and signature validation. Do not silently replace a
working shared driver or change Windows security settings.

The original project code currently has **no selected license**. The public
repository does not grant reuse rights. Third-party USBPcap code and packages
retain their own licenses. Please discuss a contribution with the maintainer
before submitting code while the project's licensing decision is pending.
