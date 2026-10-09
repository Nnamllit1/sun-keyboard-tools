# Adding a keyboard profile

Profiles describe device identity and an array-style USB keyboard report. They do
not load executable code. The application selects one profile and one matching
keyboard. A second keyboard with the same identity must be unplugged first.

Copy `profiles/sun-type7.ini` into the Profile folder opened from settings. Give
the file a new name and set values based on the actual device, not a similar
model's name. Restart the application to refresh the profile list.

```ini
[device]
name=Your keyboard model
vid=0x0430
pid=0x00a2

[report]
endpoint=0x81
size=8
keys_offset=2
keys_count=6

[keys]
0x7c=Copy|Ctrl+C
0x7d=Paste|Ctrl+V
```

The example identity and layout are for Type 7, not a universal Sun profile.
Numbers may be decimal or prefixed with `0x`. UTF-8 text, `#` and `;` comments,
and blank lines are supported. Unknown fields, duplicate usages, invalid
shortcuts, and out-of-bounds layouts are rejected. Up to 32 mappings are allowed.

- `vid`, `pid`: exact USB vendor/product IDs; no wildcard matches.
- `endpoint`: interrupt IN endpoint including its direction bit, e.g. `0x81`.
- `size`: complete USB interrupt payload length, excluding USBPcap headers.
- `keys_offset`, `keys_count`: array of one-byte HID keyboard usages in the report.
- Optional `report_id`: require the first byte of the payload to match this ID.
  If present, `size` and `keys_offset` include that byte. Do not add a synthetic
  zero report ID merely because the Windows HID API counts one.
- Keys: `usage=label|shortcut`. Labels appear in the settings window. Use
  `Disabled` to keep a key visible without triggering an action.

Shortcuts consist of zero or more of `Ctrl`, `Alt`, `Shift`, `Win`, followed by one
letter, digit, F1–F24, or named key. Named keys: Esc, Tab, Enter, Space, Backspace,
Delete, Insert, Home, End, PageUp, PageDown, Left, Right, Up, Down. Shortcuts use
Windows virtual keys and the active application's behavior; they are not text
macros. A physical modifier that conflicts with the requested shortcut causes
the action to be skipped. No shell commands or arbitrary programs are supported.

The decoder sends an action once on a new press. Repeated held reports do not
repeat shortcuts. Rollover/error usages 1–3 preserve held state until a valid
report arrives. The first report after capture starts establishes the baseline;
release held keys, or tap and release an ordinary key, before testing.

Use **Key monitor** to check extra keys without triggering actions. The developer
tool `sun-key-monitor PROFILE.ini 60 OUTPUT.txt` is a bounded equivalent and must
run elevated. It reports counts and mapped key names, never raw reports or
ordinary-key content. Close the tray application before using this tool.

For bitfield/NKRO, vendor-specific, multiple-report, or non-USB serial keyboards,
add a decoder or transport in C++. Do not claim support based only on matching
USB IDs. Include the report layout source, device IDs, and results of real
press/release, unplug/replug, and modifier tests with any proposed new profile.

This backend observes traffic; it cannot suppress the original Windows key.
Map extra usages Windows ignores. Mapping an ordinary key would retain its
original behavior as well as injecting your shortcut.

Reference: [TMK's Sun keyboard USB usage table](https://github.com/tmk/tmk_keyboard/wiki/Sun-Keyboard-Protocol#usb-hid-usages-for-sun-type-67).
