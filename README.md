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

Specification of the interface:
https://www.tinetz.at/fileadmin/user_upload/Kundenservice/pdf/Beschreibung_Kundenschnittstelle_Smart_Meter_TINETZ.pdf

Discussion about this script:
https://www.photovoltaikforum.com/thread/157476-stromz%C3%A4hler-kaifa-ma309-welches-mbus-usb-kabel/?pageNo=2#post2350873

Useful description of TiNetz frames:
https://www.gurux.fi/node/18232

This script was only tested with the above meter and the mentioned suppliers.

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

