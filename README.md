# `victron-vedirect` — NUT driver for Victron SmartShunt


**Version 0.4.0:** fixes serial receive latency. The driver now consumes every byte returned by `ser_get_buf()`, preserves partial trailing frames across polls, processes all complete frames in a read, drains bytes already queued after the first valid frame, and leaves NUT with the newest valid VE.Direct telemetry. It also shortens the initial receive window from about 2.0 s to about 1.2 s.

**Version 0.3.0:** fixed battery-capacity semantics: `battery.capacity.nominal` is rated/design Ah, `battery.capacity` is actual full-charge Ah, and live remaining Ah is published separately as `experimental.battery.capacity.remaining`.
This is an experimental native Network UPS Tools (NUT) serial driver for
Victron SmartShunt/BMV devices using the VE.Direct **Text** protocol.

It was written to fill the gap between NUT's generic `ve-direct` driver and a
battery-bank-centric NUT deployment: it maps the SmartShunt's native fields to
standard NUT battery variables that `upsc`, `upsd`, and `upsmon` can consume.

## Target

- NUT 2.8.5 (and likely nearby 2.8.x source trees)
- Victron SmartShunt/BMV with VE.Direct Text telemetry
- Linux/Unix serial device via VE.Direct-to-USB or VE.Direct-to-RS232
- 19200 baud, 8N1, no flow control

## NUT variables published

| VE.Direct | NUT variable | Conversion |
|---|---|---|
| `V` | `battery.voltage` | mV -> V |
| `I` | `battery.current` | mA -> A, signed |
| `P` (preferred), or `V` x `I` | `ups.realpower` | discharge power in W |
| `P` (preferred), or `V` x `I` | `ups.load` | discharge W / `max_load_watts` x 100 |
| configured rating | `ups.realpower.nominal` | defaults to 2000 W |
| `SOC` | `battery.charge` | per-mille -> percent |
| configured design capacity | `battery.capacity.nominal` | Ah |
| configured actual capacity | `battery.capacity` | Ah; defaults to design capacity |
| `SOC` × actual capacity | `experimental.battery.capacity.remaining` | live remaining Ah |
| `CE` | `experimental.battery.consumed_ah` | mAh -> signed Ah |
| `TTG` | `battery.runtime` | minutes -> seconds |
| `T` | `battery.temperature` | degrees C |
| `BMV` | `device.model`, `ups.model` | text |
| `SER#` | `device.serial`, `ups.serial` | text |
| `FW`/`FWE` | `ups.firmware` | text |
| `PID` | `ups.productid` | text |

`battery.charger.status` is derived from signed battery current as
`charging`, `discharging`, or `resting`.

### UPS load calculation

`ups.load` represents the percentage of the inverter's configured full-load
rating currently being supplied from the battery bank. The default is 2000 W:

    ups.load = discharge_watts / 2000 * 100

The driver prefers the SmartShunt's `P` field. If `P` is unavailable, it
calculates watts from `V * I`. Only discharge power counts as UPS load; when
the bank is charging or current is zero, `ups.load` is `0.0`. It also publishes
that discharge power as `ups.realpower` and the configured rating as
`ups.realpower.nominal`.

For example, a 1000 W battery-side draw with `max_load_watts = 2000` produces:

    ups.realpower: 1000.0
    ups.realpower.nominal: 2000
    ups.load: 50.0

Watts do not require a 12 V-to-120 V conversion: 1000 W on the DC side is
1000 W of power before inverter losses. If you need estimated AC output power
rather than battery-side input power, account for inverter efficiency outside
this driver or adjust the rating/logic accordingly. Values above the configured
rating are intentionally allowed to exceed 100%, which makes overloads visible.

If `publish_raw` is enabled, the driver additionally exposes received fields
under `experimental.ve-direct.*` (characters unsuitable for a variable name
are replaced with `_`).


## Serial freshness / outage-latency behavior

Version 0.4.0 fixes a receive-loop bug present in earlier builds. NUT's
`ser_get_buf()` may return many serial bytes in one call. Older versions of
this driver returned immediately after the first checksum-valid VE.Direct
frame, which discarded any later bytes already present in that same read.
That could leave the driver one or more SmartShunt telemetry frames behind.

The receive path now:

1. parses every byte returned by each serial read;
2. applies every complete checksum-valid VE.Direct frame in order;
3. preserves an incomplete trailing frame in parser state for the next call;
4. once a valid frame is obtained, performs zero-timeout reads to drain all
   bytes already queued by the kernel; and
5. leaves the last/newest valid frame as the state visible through NUT.

With `pollinterval = 1`, normal outage detection should therefore be limited
primarily by the SmartShunt's roughly 1 Hz Text telemetry cadence plus the
phase of the NUT poll, rather than by accumulated stale frames in the serial
receive path.

## Checksum handling

The parser consumes complete VE.Direct Text blocks and accepts a block only
when the modulo-256 sum of every byte in the block, including the single raw
`Checksum` byte, equals zero. Corrupt frames are ignored.

## OL / OB caveat

A SmartShunt measures DC battery current; it does **not** directly report
whether utility AC is present. NUT, however, normally uses:

- `OL` = input line present
- `OB` = operating on battery

For a battery-backed inverter system, this driver defaults to
`status_mode=inferred`:

- battery current below `-current_deadband` -> `OB`
- charging/resting -> `OL`

This is useful for `upsmon`, but it is an inference. If another data source
will provide the real AC-input state, set:

    status_mode = none

and combine the data at a higher layer instead.

## Low-battery shutdown

Use NUT's common `ignorelb` handling so the standard values published by this
driver determine `LB`:

    ignorelb
    override.battery.charge.low = 15
    override.battery.runtime.low = 300

With inferred `OB`, a discharging bank that falls below the configured SOC or
runtime threshold can therefore become `OB LB`, allowing `upsmon` to perform
normal coordinated shutdown.

## Install into NUT 2.8.5 source

Install build prerequisites for your distribution, obtain a NUT 2.8.5 source
or Git tree, then run:

    ./install-into-nut-source.sh /path/to/nut-2.8.5

The script copies the three driver source files into `drivers/` and adds
`victron-vedirect` to `drivers/Makefile.am`. It does not overwrite the stock
`ve-direct` driver.

Then, from the NUT source tree:

    ./autogen.sh
    ./configure --with-drivers=victron-vedirect
    make -j"$(nproc)"
    sudo make install

If building a release archive and you do not otherwise have the autotools
prerequisites, install autoconf, automake, and libtool first because changing
`Makefile.am` requires regenerating the generated build files.

## Configure

Copy/adapt `ups.conf.example`. Prefer a stable path under
`/dev/serial/by-id/` rather than `/dev/ttyUSB0`.

The NUT service account must be able to open the serial device. On many
Debian/Ubuntu systems that means membership in the `dialout` group.

Example:

    [victron]
        driver = victron-vedirect
        port = /dev/serial/by-id/usb-VictronEnergy_...
        ignorelb
        override.battery.charge.low = 15
        override.battery.runtime.low = 300
        battery_capacity_ah = 400
        # battery_actual_capacity_ah = 380
        default.battery.voltage.nominal = 12.8
        default.battery.type = LiFePO4
        status_mode = inferred
        current_deadband = 0.5
        max_load_watts = 2000

## Driver-specific options

- `status_mode=inferred` — default; infer `OL`/`OB` from battery current.
- `status_mode=none` — do not publish an inferred `OL`/`OB` state.
- `current_deadband=<A>` — charging/discharging deadband and OB threshold;
  default `0.5` A.
- `battery_capacity_ah=<Ah>` — rated/design bank capacity, published as `battery.capacity.nominal`. Default: 400 Ah.
- `battery_actual_capacity_ah=<Ah>` — measured full-charge capacity after aging, published as `battery.capacity`. Defaults to `battery_capacity_ah`. Live remaining Ah is calculated as actual capacity × SOC and published as `experimental.battery.capacity.remaining`.
- `max_load_watts=<W>` — full-load inverter rating used to calculate
  `ups.load`; default `2000` W.
- `publish_raw` — expose raw VE.Direct fields as experimental NUT variables.
- `rs232_power` — assert DTR and RTS for the isolated Victron RS-232 adapter.
  Normally unnecessary for VE.Direct-to-USB.

## Test

The protocol parser is independent of NUT and can be compiled/tested with:

    ./run-parser-tests.sh

The included test builds a valid binary-checksum VE.Direct block, verifies
field extraction, then corrupts a byte and verifies that the block is rejected.

## Power-off behavior

The SmartShunt cannot disconnect the AC load or command the inverter to power
off. `upsdrv_shutdown()` therefore reports shutdown control as unsupported.
Use `sdorder = -1` in this UPS section if your NUT shutdown sequence should
exclude the SmartShunt driver from late UPS power-off handling.

## Status

Experimental. Test it against the actual SmartShunt and your NUT build before
relying on it for unattended shutdowns. In particular, validate the inferred
`OL`/`OB` behavior during a real utility outage and restoration.
