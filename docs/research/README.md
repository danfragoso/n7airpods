# Research log

The working notes from building this, in the order things happened (2026-11-22 to 2026-11-25). They are kept
because they record **how** the Bluetooth side was figured out, what was tried, and what broke, not because they
describe the current code. **Later sections supersede earlier ones**, and several early conclusions were wrong
(for example `STATUS.md` ends with a verdict that the AAP channel is not stable; the cause turned out to be our
own USB access during tests, which switches Bluetooth off, see `STRATEGY.md`).

For how things work now, read [../ARCHITECTURE.md](../ARCHITECTURE.md) and [../PROTOCOL.md](../PROTOCOL.md).

| File | What it is |
|---|---|
| `FEASIBILITY.md` | the original question: can the nano open L2CAP PSM 0x1001 at all? |
| `FINDINGS.md` | what was verified on the device: L2CAP structures and function addresses (firmware 1.1.2 / 39A10023) |
| `DUAL-MODE.md` | what "parity with a modern Apple device" means and what is out of reach |
| `PLAN.md` | the gated rebuild plan written after the first attempt corrupted the loader; its invariants are still the rules |
| `STATUS.md` | status log through the first incident and rollback |
| `STRATEGY.md` | the long one: the flight recorder, LE listening, the settings page, and every UI iteration up to the 60 fps work |

Paths and tools mentioned in them may have moved: the reverse-engineering scripts are now in
[`../../research/tools/`](../../research/tools/), the probes in [`../../research/harness/`](../../research/harness/).
Bluetooth addresses and other identifiers have been redacted.
