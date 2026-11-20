# btdisk

**Bluetooth for any car radio with a USB port. No AUX cable, no FM transmitter, no new head unit.**

btdisk turns a Raspberry Pi Zero W into a USB stick. The radio sees a few WAV files and plays
them like any other stick. What it actually plays is whatever your phone is streaming over
Bluetooth right now.

```
 phone ──A2DP──▶  Pi Zero W  ──USB mass storage──▶  car radio
       ◀─AVRCP──   btdisk                           "Bluetooth 01.wav … 08.wav"
```

## Why it's nice

- **The radio's buttons work.** *Next* and *Previous* on the radio skip tracks on the phone.
  Pause the radio, switch source or turn the ignition off, and the phone pauses. It resumes
  when the radio starts reading again.
- **Track info on the display.** Title, artist and album go into the tags the radio reads when
  it opens a file: ID3 for MP3, and LIST/INFO for WAV with `wav_info = true`.
- **Digital end to end.** The phone's PCM goes into the files bit for bit. There is no second
  lossy step and no ground-loop hiss.
- **One cable, zero setup.** The radio's USB port powers the Pi. Pair "Car USB" once and it
  reconnects to your phone on its own every time you start the car.
- **Boots in seconds, safe to yank.** A minimal read-only Yocto image with a single ~200 KB C++
  daemon next to `bluetoothd`. There's no audio server and no Python, and cutting power at any
  moment is fine.

## Get it running

```sh
make configure && make layers   # once: Yocto caches and pinned layers
make image                      # read-only image (DEV=1 adds Wi-Fi and ssh)
make flash DISK=/dev/sdX
```

Plug the Pi's **USB** port (not PWR) into the radio. Within 3 minutes of power-on, pair your
phone with **Car USB**. No PIN is needed. Every setting is optional and documented in
[`config/btdisk.conf`](config/btdisk.conf), which is installed as `/etc/btdisk.conf`.

### Will my radio play it?

Put a 44.1 kHz 16-bit WAV on a normal stick and try it. If the radio skips the file, set
`format = mp3` (encoded with LAME, which is heavier on the Zero W). Radios that only read FAT16
get `fat = fat16`.

## How it works

The stick doesn't store anything. The FAT volume is generated sector by sector on demand:
eight 1 GiB files whose data region is mapped onto the live Bluetooth stream. btdisk works
out what you pressed from what the radio reads:

| the radio reads… | btdisk tells the phone |
|---|---|
| the next file | *Next* |
| the previous file, or the start of the current one again | *Previous* |
| nothing for 5 s | *Pause*, then *Play* when reading resumes |
| tag scans and filesystem probes at mount | nothing |

The radio reads ahead of real time. btdisk gives it a 0.8 s cushion, which is also the total
phone-to-speaker latency.

The code in [`app/src`](app/src) follows the data: `a2dp` (SBC sink on BlueZ, libsbc) →
`stream` (live PCM/MP3 chunks) → `livefile` (file offsets ↔ stream) → `player` (button logic,
tags) → `fat` → `scsi` → `ffs` (USB via FunctionFS). `bluez` handles AVRCP, pairing and
reconnects.

**Known limitation:** when a song ends on the phone and the next one starts by itself, the
display keeps the old title until the next button press, because radios only read tags when
they open a file.

## Develop

```sh
make test          # unit tests + a real-time simulated radio
make check-image   # the virtual disk through fsck.fat and 7-Zip
make cross         # build for the Pi with the image's SDK
make deploy        # push to a running DEV stick
make bench-test    # end to end on a bench Pi: phone → btdisk → USB → "radio"
```

`make bench-test` needs no car. The Pi's own kernel plays the radio through `dummy_hcd`, and
the phone can be another bench board, your real phone, a scripted fake on the PC, or a test
tone (`PHONE=bench|real|fake|tone`). Results land in `.bench/`.
