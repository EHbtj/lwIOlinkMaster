# Vendored protocol core

These sources are included under their GPLv3-compatible option. No commercial
license or paid service is used. The root project remains GPL-3.0-only.

| Source | Pinned commit | Local paths |
| --- | --- | --- |
| https://github.com/w1ne/iolinki-master | `55517bbee8849725d80c06c894d54095de5c64f1` | `src/vendor/iolinki_master/`, `src/iolinki_master/`, `third_party/iolinki-master/` |
| https://github.com/w1ne/iolinki | `11f08e963d8e8d7f7b2e6f47f5d881e3e9067168` | `src/vendor/iolinki/`, `src/iolinki/`, `third_party/iolinki/` |

`src/vendor/iolinki/` contains only the shared checksum/frame sources. Headers
are relocated from upstream `include/` to the Arduino library's `src/` include
root. The device stack itself is not linked. Original license notices are in
the source files and the two `LICENSE` files in this directory tree. Upstream's
implementation status is retained as reference; local status is in
`docs/validation.md`. Neither upstream nor this port has been verified on real
IO-Link silicon here.

## Local integration changes

- Optional `auto_configure` uses startup PAGE reads, derives the supported
  sensor configuration, writes MasterCycleTime, then enters OPERATE. Its master
  PREOPERATE state is a configuration phase: the device remains in wire STARTUP
  until DeviceOperate. No claim is made for a DevicePreoperate wire-mode service.
- Input-only Type 0/2 slice: OPERATE codes 0/4/5; unsupported capabilities are
  rejected. Streaming OD widths are 1/2 bytes. Idle cycles use an ISDU IDLE read
  (or a PAGE read without ISDU), without writing process-channel registers.
- Cycle fields widened to 16 bits in 100us units; opaque storage increased to
  1408 bytes. ABI compatibility with the upstream opaque storage is not claimed.
- Every request owns a response deadline covering TX plus RX, not just a single
  character. ISDU cycles are paced too. Deadline comparisons handle wraparound.
- Recovering a lost link invalidates identity, PD, configuration and the ISDU
  transaction. The new link performs fresh capability discovery.
- Retries restore the outgoing ISDU cursor. Service response watchdog, illegal
  ExtLength rejection, response service validation, IDLE/ABORT completion and
  invalid-PD handling are added. All of these need hardware interop validation.
- The retained fake device was extended for PAGE startup, MasterCycleTime ACK,
  232-byte/extended ISDU objects, malformed replies, busy replies and invalid PD.
  Only this fixture is used by our tests; the upstream CMocka suite is not bundled.

`local-changes.patch` records the exact changes to relocated upstream files.
`SHA256SUMS` records the normalized vendored sources used in this checkout.
Regenerate it only after reviewing any change and updating the validation record.
The protocol references IO-Link 1.1.x (the pinned upstream targets 1.1.5);
the supported sensor subset is checked against the 1.1.3 wire tables. This does
not establish conformance or interoperability with all 1.1 devices.
