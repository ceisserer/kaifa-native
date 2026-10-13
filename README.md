# Kaifa MA309 Smart Meter Logger (native C version)

## Overview

This program was made to read out the new Smart Meter "Kaifa MA309"
used by Austrian power grid operators, here tested with TINETZ and EVN.

It is a C port of the Python script `kaifareader.py` from
[tirolerstefan/kaifa](https://github.com/tirolerstefan/kaifa) and uses the
same `meter.json` config format, so it can be used as a drop-in replacement.
The port was made to get rid of the Python runtime and its libraries
(e.g. paho-mqtt 2.x broke the original script), and to run as a single small
binary with low memory and CPU usage on small devices like a Raspberry Pi
or a router.

The project consists of two parts:

- **`kaifareader`**: a ready-to-use daemon that exports the meter values
  to MQTT or to a Solarview file (see [Config](#config) and following).
- **`libkaifa`**: the meter readout itself (serial port, telegram framing,
  decryption, OBIS decoding) as a C library with a small interface, for
  integrating the meter into your own programs. It only depends on OpenSSL's
  libcrypto, no libmosquitto or cJSON needed. See
  [Shared library (libkaifa)](#shared-library-libkaifa).

Specification of the interface:
https://www.tinetz.at/fileadmin/user_upload/Kundenservice/pdf/Beschreibung_Kundenschnittstelle_Smart_Meter_TINETZ.pdf

Discussion about this script:
https://www.photovoltaikforum.com/thread/157476-stromz%C3%A4hler-kaifa-ma309-welches-mbus-usb-kabel/?pageNo=2#post2350873

Useful description of TiNetz frames:
https://www.gurux.fi/node/18232

This program was only tested with the above meter and the mentioned suppliers.

## Required Hardware

![Picture of wiring](img/connection.png)

1. RJ12 6P6C Plug

    e.g. https://www.amazon.de/6P6C-Stecker-6-polige-Schraubklemmen-Adapterstecker-CCTV-Adapterstecker/dp/B07KFGS3BF
<br><br>
    **Note**: you have to cut off the plastic shell of the RJ12 plug to fit it into the socket of the MA309.
<br><br>
2. MBUS Slave module like this:

    https://www.amazon.de/JOYKK-USB-zu-MBUS-Slave-Modul-Master-Slave-Kommunikation-Debugging-Bus%C3%BCberwachung/dp/B07PDH2ZBV

## Config

Create a file `meter.json` to configure your serial connection 
parameters and your AES key.

A template file `meter_template.json` can be recycled for this.

```
{
  "loglevel": "logging.INFO",
  "logfile": "/var/log/kaifareader/kaifa.log",
  "port": "/dev/ttyUSB0",
  "baudrate": 2400,
  "parity": "serial.PARITY_NONE",
  "stopbits": "serial.STOPBITS_ONE",
  "bytesize": "serial.EIGHTBITS",
  "key_hex_string": "",
  "interval": 15,
  "supplier": "TINETZ",
  "export_format": "SOLARVIEW",
  "export_file_abspath": "/var/run/kaifareader/kaifa.txt",
  "export_mqtt_server": "mymqtt.examplebroker.com",
  "export_mqtt_port": 1883,
  "export_mqtt_user": "mymqttuser",
  "export_mqtt_password": "supersecretmqttpass",
  "export_mqtt_basetopic": "kaifareader"
}
```

The AES key format is "hex string", e.g. `a4f2d...`

Please provide your electricity supplier by the field "supplier". Because each supplier uses its own security standard, 
the telegrams differ. This script was tested with suppliers:
- TINETZ
- EVN

### Export

**Solarview**

Currently, the export to a file readable by Solarview (http://solarview.info/)
is supported.

- The config key `export_format` has to be set to `SOLARVIEW`
- The config key `export_file_abspath` has to be set to the absolute file path

**MQTT**

- The config key `export_format` has to be set to `MQTT`
- The config keys `export_mqtt_server`, `export_mqtt_port`,
  `export_mqtt_user`, `export_mqtt_password` and `export_mqtt_basetopic`
  have to be set.

The values are published below `<export_mqtt_basetopic>/`. Which values are
published, and under which topic names, can be set with the optional config
key `export_mqtt_topics`, which maps topic names to value names:

```
  "export_mqtt_topics": {
    "SpannungL1": "voltage_l1",
    "WirkenergieP": "energy_in",
    "Momentanleistung": "power_net"
  }
```

Available values:

| Value | Description |
|---|---|
| `energy_in_kwh` | active energy imported (kWh, OBIS 1.8.0 / 1000) |
| `energy_out_kwh` | active energy exported (kWh, OBIS 2.8.0 / 1000) |
| `energy_in` | active energy imported (Wh, OBIS 1.8.0) |
| `energy_out` | active energy exported (Wh, OBIS 2.8.0) |
| `voltage_l1`..`voltage_l3` | voltage per phase (V) |
| `current_l1`..`current_l3` | current per phase (A) |
| `power_in` | active power imported (W, OBIS 1.7.0) |
| `power_out` | active power exported (W, OBIS 2.7.0) |
| `power_net` | net active power, `power_in - power_out` (W) |
| `power_factor` | power factor |

Without `export_mqtt_topics`, these topics are published:

| Topic | Value |
|---|---|
| `RealEnergyIn_S` | `energy_in_kwh` (same as the original Python version) |
| `RealEnergyOut_S` | `energy_out_kwh` (same as the original Python version) |
| `VoltageL1`..`VoltageL3` | `voltage_l1`..`voltage_l3` |
| `CurrentL1`..`CurrentL3` | `current_l1`..`current_l3` |
| `RealEnergyIn` | `energy_in` |
| `RealEnergyOut` | `energy_out` |
| `RealPowerIn` | `power_in` |
| `RealPowerOut` | `power_out` |
| `RealPower` | `power_net` |
| `PowerFactor` | `power_factor` |

## Build

Build dependencies (Debian/Raspberry Pi OS):

`sudo apt install build-essential libssl-dev libmosquitto-dev libcjson-dev`

Build and install to `/usr/bin/kaifareader`:

```
make
sudo make install
```

`kaifareader` contains libkaifa (linked statically), so the library does not
have to be installed to run it. To build the library, see below.

## Shared library (libkaifa)

### Build and install

The library only needs OpenSSL's libcrypto:

`sudo apt install build-essential libssl-dev`

```
make lib
sudo make install-lib
```

This builds and installs

| File | |
|---|---|
| `/usr/lib/libkaifa.so.1.0.0` (+ symlinks `libkaifa.so.1`, `libkaifa.so`) | shared library |
| `/usr/lib/libkaifa.a` | static library |
| `/usr/include/kaifa.h` | header, documents the whole interface |
| `/usr/lib/pkgconfig/kaifa.pc` | pkg-config file |

Use `PREFIX=...`, `LIBDIR=...` and `DESTDIR=...` to change the install
locations, e.g. `sudo make install-lib PREFIX=/usr/local` or
`make install-lib LIBDIR=/usr/lib/aarch64-linux-gnu` for multiarch.

Link your program with

```
cc -o myprog myprog.c $(pkg-config --cflags --libs kaifa)
```

or for static linking `$(pkg-config --static --cflags --libs kaifa)`.

### Usage

```c
#include <stdio.h>
#include <kaifa.h>

int main(void)
{
    kaifa_config_t cfg;
    kaifa_config_init(&cfg);              /* defaults: 2400 8N1, TINETZ, ... */
    cfg.port = "/dev/ttyUSB0";
    cfg.key_hex = "00112233445566778899aabbccddeeff";
    cfg.supplier = KAIFA_SUPPLIER_TINETZ; /* or KAIFA_SUPPLIER_EVN */

    kaifa_reader_t *reader;
    int rc = kaifa_open(&cfg, &reader);
    if (rc != KAIFA_OK) {
        fprintf(stderr, "kaifa_open: %s\n", kaifa_strerror(rc));
        return 1;
    }

    kaifa_reading_t r;
    for (;;) {
        rc = kaifa_read(reader, &r, 30000); /* timeout in ms, -1 = forever */
        if (rc == KAIFA_ERR_IO || rc == KAIFA_ERR_ABORTED)
            break;
        if (rc != KAIFA_OK) /* KAIFA_ERR_TIMEOUT, KAIFA_ERR_FRAME: try again */
            continue;
        if (r.values[KAIFA_POWER_IN].valid)
            printf("power in: %.0f W\n", r.values[KAIFA_POWER_IN].value);
    }

    kaifa_close(reader);
    return 0;
}
```

A complete example that prints all values is in
[`examples/kaifa_example.c`](examples/kaifa_example.c)
(`make example`, then
`LD_LIBRARY_PATH=. examples/kaifa_example /dev/ttyUSB0 <key> [TINETZ|EVN]`).

### Interface overview

| Function | Description |
|---|---|
| `kaifa_config_init(&cfg)` | fill a `kaifa_config_t` with defaults |
| `kaifa_open(&cfg, &reader)` | open and configure the serial port |
| `kaifa_read(reader, &reading, timeout_ms)` | wait for the next telegram pair, decrypt and decode it |
| `kaifa_abort(reader)` | make a blocking `kaifa_read()` return `KAIFA_ERR_ABORTED`; safe from other threads and signal handlers |
| `kaifa_close(reader)` | close the port and free the reader |
| `kaifa_decode(&cfg, frame1, len1, frame2, len2, &reading)` | decode a telegram pair received by other means (no serial port) |
| `kaifa_value_name(id)`, `kaifa_value_unit(id)`, `kaifa_value_obis(id)` | metadata of a value, e.g. `"voltage_l1"`, `"V"`, `"1.0.32.7.0.255"` |
| `kaifa_strerror(rc)` | text for a `KAIFA_*` return code |

`kaifa_config_t` fields:

| Field | Default | Description |
|---|---|---|
| `port` | - | serial device, required |
| `key_hex` | - | AES key as hex string (32, 48 or 64 digits), required |
| `supplier` | `KAIFA_SUPPLIER_TINETZ` | grid operator, `KAIFA_SUPPLIER_TINETZ` or `KAIFA_SUPPLIER_EVN` |
| `baudrate`, `parity`, `stopbits`, `bytesize` | `2400`, `'N'`, `1`, `8` | serial settings |
| `chunk_timeout` | `1.0` | seconds to wait for a chunk of serial data (`interval` in `meter.json`) |
| `log_fn`, `log_user`, `log_level` | `NULL`, `NULL`, `KAIFA_LOG_INFO` | optional log callback; the library itself never prints anything |

A `kaifa_reading_t` contains `values[KAIFA_VALUE_COUNT]`, indexed by:

| Id | Unit | OBIS |
|---|---|---|
| `KAIFA_VOLTAGE_L1`..`KAIFA_VOLTAGE_L3` | V | 1.0.32.7.0.255, 1.0.52.7.0.255, 1.0.72.7.0.255 |
| `KAIFA_CURRENT_L1`..`KAIFA_CURRENT_L3` | A | 1.0.31.7.0.255, 1.0.51.7.0.255, 1.0.71.7.0.255 |
| `KAIFA_POWER_IN` / `KAIFA_POWER_OUT` | W | 1.0.1.7.0.255 / 1.0.2.7.0.255 |
| `KAIFA_ENERGY_IN` / `KAIFA_ENERGY_OUT` | Wh | 1.0.1.8.0.255 / 1.0.2.8.0.255 |
| `KAIFA_POWER_FACTOR` | | 1.0.13.7.0.255 |

Each value has `valid` (false if missing in the telegram), `value` (as
`double`, in the unit above) and the transmitted `raw` value and decimal
`scaler`. The reading also contains the meter's `system_title` and the
telegram's `invocation_counter`.

Notes:

- A reader must only be used by one thread at a time (except
  `kaifa_abort()`); multiple readers for multiple meters are fine.
- After `kaifa_abort()` all further `kaifa_read()` calls on that reader
  return `KAIFA_ERR_ABORTED`; close it and open a new one to continue.
- Like the original Python script, the AES-GCM authentication tag is not
  verified, so a wrong key yields garbage values rather than an error.

## Installation

### Debian package

Build the package (additionally needs `debhelper`):

```
cd dpkg
./build_pkg.sh
```

### Systemd automatic service

This installs and automatically starts a systemd service.

Install the debian package 

`sudo dpkg -i kaifareader_...deb`

If there are problems on missing packages, execute afterwards:

`sudo apt -f install`

## Start

### Automatically, if service is installed and running

Startup done, automatically.

Status of the service:

`sudo systemctl status kaifareader`

Start manually (e.g. after manually stopped)

`sudo systemctl start kaifareader`

### Manually

#### Foreground

`kaifareader [config_file]` (default: `/etc/kaifareader/meter.json`)

#### Background

`nohup kaifareader /etc/kaifareader/meter.json &`

## Stop

### If service is installed and running

`sudo systemctl stop kaifareader`

### Foreground

Press CTRL+C

### Background

Possible by killing the process

