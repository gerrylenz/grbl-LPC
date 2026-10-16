# grbl-LPC für die Lasermaschine

Fork von [cprezzi/grbl-LPC](https://github.com/cprezzi/grbl-LPC) (Grbl 1.1f auf dem LPC1768/LPC1769). Diese Fassung spricht mit LightBurn über die RJ45-Buchse, nutzt das Maschinenprofil der Laseranlage und kann mit einem analogen Joystick verfahren.

Die allgemeinen Grbl-Befehle stehen im [Grbl-Wiki](https://github.com/gnea/grbl/wiki). Lizenz: GPLv3, siehe `COPYING`.

## Deutsch

### LightBurn über Ethernet

Die USB-Serielle Schnittstelle wird nicht gestartet. Der Grbl-Datenstrom läuft als rohes TCP auf Port 23, ohne Telnet-Steuerzeichen.

| | |
|---|---|
| IP | `192.168.178.222` |
| Port | `23` |
| MAC | `02:47:52:42:4C:01` |

Adresse und MAC stehen in `enet/net_config.h`. PC und Board müssen im Netz `192.168.178.0/24` liegen. Ist die Adresse belegt, den Wert dort ändern und neu bauen.

In LightBurn: Gerätetyp **GRBL**, Verbindung **Ethernet or Wifi/TCP**, diese IP, Port 23. S-Value Max muss zu `$30` passen, hier **1000**. Zuerst Übertragung **Synchronous**. Ein Ping auf die IP prüft die Leitung, bevor LightBurn verbunden wird.

`firmware.bin` auf die SD-Karte ins Stammverzeichnis kopieren und die Karte zurücksetzen. `build/grbl.hex` ist nur für ein direktes Flashen ohne SD-Bootloader.

### Maschinenprofil

Gebaut wird die Map `CPU_MAP_MKS_SBASE` mit den Defaults `DEFAULTS_Laser`. Die Zahlen stehen auch in `grbl-config.txt`. Sie gelten auf der Maschine erst nach `$RST=*`. Bis dahin bleibt der Inhalt des EEPROM.

| | |
|---|---|
| Schritte X/Y/Z/A | 313,482 / mm |
| Vorschub | 12 000 mm/min |
| Beschleunigung | 2500 mm/s² |
| Arbeitsraum X/Y/Z | 600 / 400 / 150 mm |
| Strom X/Y | 0,6 A, Z und A aus |
| Richtung `$3` | 3 (X und Y invertiert) |
| Homing `$22` / `$23` | an / 1 |
| Softlimits `$20` / Hardlimits `$21` | an / aus |
| Laser `$30` / `$32` | 1000 / an |
| Homing-Ursprung | am angefahrenen Schalter |

### Pins

| Funktion | Pin |
|---|---|
| Flut (M8) | P2.7 |
| Nebel (M7) | P2.6 |
| Laser-PWM | P2.5 |
| Spindel-Enable | P1.30 |
| Spindel-Richtung | P1.31 |
| Feed-Hold | P1.22 |
| Cycle-Start | P1.23 |
| Probe | P1.27 (Y-Max) |
| Joystick X | TH2, P0.24 |
| Joystick Y | TH3, P0.25 |

M7 und M8 können gleichzeitig aktiv sein.

### Joystick

Die Buchsen **TH2** und **TH3** an der MKS SBASE haben nur zwei Pole, **+** und **−**. **TH1** (P0.23) bleibt frei. **+** ist der Messeingang, auf der Platine liegt daran ein Widerstand nach 3,3 V. **−** ist Masse. An **+** keine 5 V legen.

Den mittleren Kontakt des Potis an **+**, einen Außenkontakt an **−**. Den anderen Außenkontakt offen lassen oder mit der Mitte verbinden. Beim Einschalten muss der Stick losgelassen in der Mitte stehen. Diese Lage wird als Nullpunkt gespeichert. Außerhalb einer Totzone fährt die Maschine in kurzen Stücken, die Geschwindigkeit folgt der Auslenkung. In der Mitte wird der Jog sofort abgebrochen. Der Laser bleibt dabei aus.

Der Stick wirkt nur im Leerlauf. Während eines LightBurn-Jobs bleibt er ohne Wirkung. Weil Homing an ist, startet die Steuerung im Alarm. Der Stick greift erst nach `$H` oder `$X`. Läuft eine Achse falsch herum, `JOYSTICK_INVERT_X` oder `JOYSTICK_INVERT_Y` in `grbl/joystick.c` auf 1 setzen.

### Bauen

```
git submodule update --init
make
```

Benötigt werden `make` und die ARM-Toolchain `arm-none-eabi-gcc`. Das Ergebnis ist `build/firmware.bin`.

## English

Fork of [cprezzi/grbl-LPC](https://github.com/cprezzi/grbl-LPC) (Grbl 1.1f on the LPC1768/LPC1769). This build talks to LightBurn through the RJ45 jack, uses this laser machine's profile, and can jog from an analog joystick.

General Grbl commands are in the [Grbl wiki](https://github.com/gnea/grbl/wiki). License: GPLv3, see `COPYING`.

### LightBurn over Ethernet

USB serial is not started. The Grbl byte stream is raw TCP on port 23, with no Telnet control codes.

| | |
|---|---|
| IP | `192.168.178.222` |
| Port | `23` |
| MAC | `02:47:52:42:4C:01` |

Address and MAC are in `enet/net_config.h`. The PC and the board must be on `192.168.178.0/24`. If that address is taken, change it there and rebuild.

In LightBurn: device type **GRBL**, connection **Ethernet or Wifi/TCP**, that IP, port 23. S-Value Max must match `$30`, which is **1000** here. Start with **Synchronous** transfer. Ping the IP before connecting LightBurn.

Copy `firmware.bin` to the root of the SD card and reset the board. `build/grbl.hex` is only for flashing without the SD bootloader.

### Machine profile

The build uses `CPU_MAP_MKS_SBASE` and `DEFAULTS_Laser`. The same numbers are listed in `grbl-config.txt`. They replace the values stored on the machine only after `$RST=*`. Until then the EEPROM contents stay in effect.

| | |
|---|---|
| Steps X/Y/Z/A | 313.482 / mm |
| Feed | 12 000 mm/min |
| Acceleration | 2500 mm/s² |
| Travel X/Y/Z | 600 / 400 / 150 mm |
| Current X/Y | 0.6 A, Z and A off |
| Direction `$3` | 3 (X and Y inverted) |
| Homing `$22` / `$23` | on / 1 |
| Soft limits `$20` / hard limits `$21` | on / off |
| Laser `$30` / `$32` | 1000 / on |
| Homing origin | at the switch that was reached |

### Pins

| Function | Pin |
|---|---|
| Flood (M8) | P2.7 |
| Mist (M7) | P2.6 |
| Laser PWM | P2.5 |
| Spindle enable | P1.30 |
| Spindle direction | P1.31 |
| Feed hold | P1.22 |
| Cycle start | P1.23 |
| Probe | P1.27 (Y max) |
| Joystick X | TH2, P0.24 |
| Joystick Y | TH3, P0.25 |

M7 and M8 may be on at the same time.

### Joystick

The **TH2** and **TH3** sockets on the MKS SBASE have two pins, **+** and **−**. **TH1** (P0.23) stays unused. **+** is the sense input; the board already has a resistor from that pin to 3.3 V. **−** is ground. Do not feed 5 V into **+**.

Connect the pot wiper to **+** and one outer pin to **−**. Leave the other outer pin open, or tie it to the wiper. The stick must be released at the center when power is applied. That reading is stored as the zero point. Outside a dead zone the machine jogs in short segments, and the feed follows the deflection. At the center the jog is cancelled at once. The laser stays off.

The stick works only while idle. It has no effect during a LightBurn job. Homing is enabled, so the controller starts in alarm. The stick works after `$H` or `$X`. If an axis runs the wrong way, set `JOYSTICK_INVERT_X` or `JOYSTICK_INVERT_Y` to 1 in `grbl/joystick.c`.

### Build

```
git submodule update --init
make
```

This needs `make` and the ARM toolchain `arm-none-eabi-gcc`. The output is `build/firmware.bin`.
