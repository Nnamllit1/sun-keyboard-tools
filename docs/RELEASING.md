# Build and release

Normal users download the graphical installer from [Releases](https://github.com/Nnamllit1/sun-keyboard-tools/releases/latest). The commands here are for maintainers.

`VERSION` is the source of truth for the application, Windows executable metadata,
installer filename, and release tag. `tools/package.ps1` downloads the exact
USBPcap release dependencies, verifies their SHA-256 values and Authenticode,
builds the application and setup, runs CTest, and writes the installer, checksum,
and release notes to `out`. `tests/package-smoke.ps1` checks the packaged files
and version metadata without installing a driver.

## Publish a version

1. Update `VERSION` and add `docs/releases/<version>.md` with user-facing changes
   and relevant compatibility limitations.
2. Push the changes to `main`; wait for **Windows build** to pass. Pull requests
   also build and test the full installer, with downloadable Actions artifacts.
3. Create and push the matching tag, for example `v0.2.0`:

   ```sh
   git tag -a v0.2.0 -m "Sun Keyboard Tools 0.2.0"
   git push origin v0.2.0
   ```

4. **Build and release** rebuilds and verifies that tag, checks that the tag matches
   `VERSION`, uploads the installer and checksum to a draft, then makes the
   complete release public. The job uses GitHub's built-in token; no personal
   access token or custom release secret is required.

Rerun a failed release job from Actions. An existing draft can be completed on a
retry; an already-public release will not have its assets silently replaced.
For **Run workflow**, select the existing version tag, not `main`.

GitHub Actions run with read-only repository permissions for builds and pull
requests. Only the publishing job has contents-write permission. Actions are
pinned by commit; Dependabot proposes updates monthly. CI does not install or
remove kernel drivers. Driver lifecycle testing belongs on a Windows test system.

Do not remove the bundled corresponding USBPcap source or license notices from
the installer. The project's original source remains without a selected license
until the owner chooses one. A public repository does not change that decision.
