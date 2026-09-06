# Changelog

## 0.4.0

- Fixed serial-buffer data loss after the first valid VE.Direct frame in a read.
- The driver now processes all bytes and all complete frames returned by `ser_get_buf()`.
- Partial trailing frames persist across subsequent reads/polls.
- After receiving a valid frame, the driver drains bytes already queued with zero-timeout reads so the newest available frame wins.
- Reduced the fresh-frame wait window from about 2.0 seconds to about 1.2 seconds.
- Added regression coverage for back-to-back charging/discharging frames and split frames across poll boundaries.

## 0.3.0

- Corrected NUT battery-capacity semantics and added remaining-Ah telemetry.

## 0.2.0

- Added `ups.load`, `ups.realpower`, and configurable `max_load_watts`.

## 0.1.0

- Initial SmartShunt VE.Direct native NUT driver.
