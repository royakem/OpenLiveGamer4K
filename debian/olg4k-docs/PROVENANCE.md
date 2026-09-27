# Bounded provenance report

**Scope:** source comparison for scaler coefficients and initialization code in
this repository against `ostrich/gc555` revision
`7bb51508742adf750b17756b2211ca2bb6eeb361`, the revision named in
`THIRD_PARTY_NOTICES.md`. This records source-level matches and differences;
it does not assess topics beyond this scope.

## Findings

### Scaler coefficient table

`src/gc573_scaler_coeff.h` and `gc555-fpga.c`'s
`gc555_fpga_six_tap_coeff` match exactly: **64 rows × 6 signed values = 384
values; 0 differing values**. Coefficients are written as three packed pairs
per phase at base + `0x800`; the vertical and horizontal bases are `0x60000`
and `0x40000`, respectively.

The setup order and phase-word construction in `src/gc573_scaler.c` follow the
reference scaler routines. The phase-table sizing expressions differ:
upstream uses `DIV_ROUND_UP(max(widths), 4)`; this code uses integer division
by four. They select the same entries for this driver's currently accepted
widths, all divisible by four. This is a bounded source comparison and does
not establish the history of the matching values in any GC573 vendor object.

### IT6805 initialization table

The active `gc573_it6805_initial_sequence` in
`src/gc573_it6805_sequence.h` has 114 ordered `(register, mask, value)`
triples. It matches the pinned `it6805_initial_sequence` in
`gc555-it6805-core.c` after excluding two upstream triples:

- `{ 0x42, 0xe0, 0xc0 }` in bank 0x04.
- `{ 0x42, 0xe0, 0xc0 }` in bank 0x00.

The remaining 114 triples match in order and value. The original decision for those omissions was not recorded. An earlier review
proposed an audio-related explanation, but it was an inference and is not
established hardware documentation. Current stereo audio works with the
omissions unchanged; this observation does not establish their purpose. The surrounding GC573 receiver lifecycle also
contains board-specific operations and is not asserted to be a verbatim copy
of upstream's complete lifecycle.

### IT6664 initialization adaptations

`src/gc573_bridge_clock.c` follows the pinned switch pre-RCLK, SIPROM and RCLK
calibration routines in `gc555-it6664-core.c`: the operation order and
programmed values correspond. GC573-specific I2C access and added bank
restoration on error paths make this an adaptation rather than identical
source or an asserted identical transaction trace.

`src/gc573_bridge_rx.c` adapts the reference CAOF, RX register and power-state
initialization routines. In particular, CAOF timeout behavior differs: the
reference proceeds after its recovery toggle, while this code restores bank 0
and returns `-ETIMEDOUT`. The complete RX transaction sequence is not claimed
to match write-for-write. Board identity, addresses and topology are outside
what the upstream source comparison can establish.

## Completed follow-up review

The unused `gc573_receiver_init_sequence.h` was traced to observed vendor I2C
transactions and removed from the candidate. It is not built or shipped in the
current source or public Git history.
The research traces and vendor objects themselves are not distributed.

The source comparison confirmed all 384 scaler values against the pinned GPL source
and separately against the private vendor-object symbol. The distributable
source basis is the attributed GPL table; vendor objects are not build inputs.
The newer IT6805 audio clock path follows the pinned upstream N/CTS/TMDS
measurement and output setup logic, adapted to GC573 bank access, stable 48 kHz
validation, cleanup, and error handling. Attribution is retained in
THIRD_PARTY_NOTICES.md. This is not described as a clean-room implementation.

## Remaining hardware qualification

The two omitted writes remain documented differences, not independently
understood register behavior. Broader receiver/scaler behavior, timeout
recovery, long-duration operation and A/V synchronization remain qualification
work. Source provenance comparison does not establish those hardware results.
