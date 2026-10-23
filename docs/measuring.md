# How to measure

* **Clock:** COP0 Count (`mx_count()`) runs at the EE clock, 294.912 MHz, and wraps every 14.6 s.
  Differences of `u32` values are correct across one wrap.
* **Performance counters:** `mx_pccr_write()` selects two events (PCCR, COP0 reg 25), `mx_pcr0()`
  / `mx_pcr1()` read them. Event numbers are in the EE Core User's Manual, ch. 9.
* **Probe template:** one ELF per question in `probe/`. It runs every variant several times
  (first run = cold cache, report it separately), prints `name value unit` lines, holds the debug
  output during the timed part (`mx_debug_hold(1)`, so SIF traffic does not disturb the timing) and
  ends with `MX_TEST_END(status)`.
* **Where:** correctness on PCSX2 first (`ps2-pcsx2-test` skill). Timing only on the PS2, in as few
  runs as possible (one probe ELF collects many measurements). PCSX2 timings are never used.
* **Interrupts and DMA:** handlers record `mx_count()` into a trace ring (`mx_trace`), so latencies
  are measured from the event to the first handler instruction, and gaps between DMA segments from
  one segment's end interrupt to the next segment's first transfer.
* **Disturbances:** SIF DMA (debug output), the BIOS's own interrupt handlers (vsync, timers) and
  cache state. Report minimum and median of repeated runs.
