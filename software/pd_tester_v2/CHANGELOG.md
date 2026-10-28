# USB PD Tester – Firmware v2.0 (USB PD 3.1)

## New features
- **EPR mode (Extended Power Range, PD 3.1):** the tester automatically enters EPR mode if the
  source advertises it (EPR_Mode Enter → Enter Acknowledged → Enter Succeeded), receives the
  chunked `EPR_Source_Capabilities` (up to 13 PDOs, 2 chunks), requests with `EPR_Request`
  (RDO + PDO copy) and keeps the mode alive with `EPR_KeepAlive` every 375 ms.
  The reason of a failed entry (e.g. non-EPR cable) is shown on the display.
- **AVS (Adjustable Voltage Supply, 15…48 V):** AVS APDOs are decoded (min/max voltage, PDP)
  and can be adjusted in 100 mV steps; the requested current is calculated from PDP/V
  (capped at 5 A).
- **Voltage limit:** `PD_VOLTAGE_LIMIT` in `config.h` (default 20000 mV). PDOs above the limit
  are shown but locked (`#`), PPS/AVS ranges are capped. Further options: `PD_EPR_ENABLE`,
  `PD_EPR_SINK_PDP`, `PD_PPS_STEP`, `PD_AVS_STEP`.
- Variable and battery PDOs are decoded and can be requested.
- Answers `Get_Sink_Cap` (Sink_Capabilities), `Get_Revision` (Revision 3.1) and replies
  `Not_Supported` to unsupported messages (PD 3.x).
- New UI: scrolling list for up to 13 PDO positions, type letter per PDO (F/V/B/P/A),
  status line with PD revision, EPR state and voltage limit, result messages
  (OK/Rejected/Wait/Timeout), adjust screen with live current for AVS, key repeat with
  acceleration (x10 after 2 s).

## Bug fixes
- `PD_PDO_analyze()` wrote fixed PDOs to `PD_SC_fixed[i]` (5 entries) and PPS to
  `PD_SC_PPS[]` (2 entries): buffer overflow with sources offering 6–7 fixed PDOs or 3+ PPS.
- Variable, battery and EPR APDOs were interpreted as fixed PDOs (wrong values/requests).
- `Reject`/`Wait` were not handled: the state machine stayed in `CC_WAIT_ACCEPT` forever and
  all later requests failed.
- `Soft_Reset` from the source was not answered (no Accept) → source hard reset.
- Lost GoodCRC was never retried, retransmissions from the source were processed twice
  (no MessageID duplicate filter).
- Interrupt flags were cleared with `STATUS |= flag`, which also cleared other pending flags.
- Main loop called `PD_negotiate()` every 10 ms, which sent `Get_Source_Cap` and re-negotiated
  the contract permanently (bus load, blocking up to 1.3 s). Now only a PPS refresh request
  every 5 s (tPPSRequest) is sent.
- `keydelay` in `main()` was used uninitialized on the first key press in the PPS screen.
- `ch32x035.h`: `USBPD_TX_HARD_RESET` referenced the undefined `USBPD_TX_SEL4_RST2`.
- Misaligned 32-bit accesses on the PD buffers replaced by byte access.
- `EPRModeCapable` RDO bit is only set for PD 3.x (reserved in PD 2.0).

## Robustness
- Interrupt only does time critical work (GoodCRC, duplicate filter, receive queue);
  the policy engine runs in `PD_update()` in the main context.
- Timeouts per spec: tTypeCSinkWaitCap (Hard Reset if no caps, max. 2x), tSenderResponse,
  tPSTransition (SPR/EPR), tEnterEPR, chunk timeout.
- Collision avoidance (PD 3.x): the sink only initiates an AMS when Rp = 3.0 A (SinkTxOK).
- `makefile` detects the toolchain (`riscv64-unknown-elf` or xPack `riscv-none-elf`) and adds
  `_zicsr` for GCC ≥ 12.

## API changes (`usbpd_sink.h`)
- New: `PD_update()`, `PD_getPDOType()`, `PD_getPDOMaxPower()`, `PD_getPDOLimitVoltage()`,
  `PD_isPDOSelectable()`, `PD_getVoltageStep()`, `PD_getAVSNum()`, `PD_isConnected()`,
  `PD_getCapsVersion()`, `PD_getResult()`, `PD_getRevision()`, `PD_isEPRCapable()`,
  `PD_isEPR()`, `PD_getEPRState()`, `PD_getEPRFail()`.
- `PD_update()` must be called at least every 5 ms (EPR keep alive, PPS refresh, responses).
- PDO positions are the object positions of the source: in EPR mode 8…13 are EPR PDOs,
  unused SPR slots are `PD_PDO_EMPTY`.

## Hardware note
The original board is **not** rated for EPR voltages: C3 (330 nF, 25 V) and the 78L05
(30 V max.). With `PD_VOLTAGE_LIMIT = 20000` the tester can still enter EPR mode and display
all EPR capabilities, but only selects voltages up to 20 V. For 28/36/48 V replace C3 by a
≥ 63 V (better 100 V) part and the 78L05 by a regulator rated for ≥ 55 V input, and use a
5 A EPR cable.
