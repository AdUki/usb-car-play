# btdisk

**Bluetooth for any car radio with a USB port. No AUX cable, no FM transmitter, no new head unit.**

btdisk turns a Raspberry Pi Zero W into a USB stick. The radio sees a few MP3 files and plays
them like any other stick. What it actually plays is whatever your phone is streaming over
Bluetooth right now.

```
 phone ──A2DP──▶  Pi Zero W  ──USB mass storage──▶  car radio
       ◀─AVRCP──   btdisk                           "Bluetooth 01.mp3 … 16.mp3"
```

## Why it's nice

- **The radio's buttons work.** *Next* and *Previous* on the radio skip tracks on the phone.
  Pause the radio, switch source or turn the ignition off, and the phone pauses. It resumes
  when the radio starts reading again.
- **The phone's name on the display.** The tags the radio reads when it opens a file (ID3 for
  MP3, LIST/INFO for WAV with `wav_info = true`) carry the phone's name. A radio reads them only
  when it opens a file, so the song's own title would go stale as soon as the next song started;
  `tags = track` puts it there anyway.
- **Digital end to end.** No ground-loop hiss: 256 kbps MP3 by default, or the phone's PCM
  untouched in WAV files for radios that play them. The phone can send AAC or aptX as well as
  SBC (aptX HD too, with `codecs` in the config).
- **Clocks kept in step.** The phone's clock and the radio's are tracked and the difference is
  resampled away, so a long drive doesn't end in a skip or a dropout.
- **One cable, zero setup.** The radio's USB port powers the Pi. Pair "Car USB" once and it
  reconnects to your phone on its own every time you start the car.
- **Boots in seconds, safe to yank.** A minimal read-only Yocto image with a single ~200 KB C++
  daemon next to `bluetoothd`. There's no audio server and no Python, and cutting power at any
  moment is fine.

## Get it running

```sh
make configure && make layers   # once: Yocto caches and pinned layers
make image                      # read-only image (DEV=1 adds Wi-Fi and ssh, TRACE=1 a car log)
make flash DISK=/dev/sdX
```

Plug the Pi's **USB** port (not PWR) into the radio. Within 3 minutes of power-on, pair your
phone with **Car USB**. No PIN is needed. Every setting is optional and documented in
[`config/btdisk.conf`](config/btdisk.conf), which is installed as `/etc/btdisk.conf`.
After power-on it calls the phone that was connected last, every 5 s for the first 3 minutes.

The image writes to the SD card only when a phone pairs or a different one connects (BlueZ's
pairings and the last phone, on the data partition): the log stays in RAM. To find out what
goes wrong in the car, build with `make image TRACE=1`: then the data partition keeps the
story across power cuts. `btdisk.log` (and `.1`) has what the radio read and when it paused,
what was sent to the phone and how long it took, the muting around it and the Bluetooth
connections, every drive numbered (`run N`); `syslog.log` has bluetoothd's and the kernel's.

### Will my radio play it?

MP3 is the default, because older head units often play nothing else. If yours plays a
44.1 kHz 16-bit WAV from a normal stick, `format = wav` skips the encoder: lossless, and much
lighter on the Zero W. Radios that only read FAT16 get `fat = fat16`.

## How it works

The stick doesn't store anything. The FAT volume is generated sector by sector on demand:
sixteen 512 MiB files whose data region is mapped onto the live Bluetooth stream. btdisk works
out what you pressed from what the radio reads:

| the radio reads… | btdisk tells the phone |
|---|---|
| a file up to 7 further on (wrapping around) | that many *Next* |
| a file up to 8 before | that many *Previous* |
| the start of the current file again, after `restart_threshold_s` | *Previous* (the phone restarts its track), unless the phone's track only just started |
| its file again after a pause or another source | nothing: it carries on where it was |
| nothing for 5 s | *Pause*, then *Play* when reading resumes |
| tag scans and filesystem probes at mount | nothing |

Several *Next* or *Previous* go one at a time, each after the phone has switched tracks:
phones drop the ones that come too fast. A *Previous* that only restarted the track is sent
again. The new file starts on silence, and the phone stays muted until it has switched and the
radio streams the file, then fades in: no tail of the old song, and no jump where a radio pauses
after its first read.

The radio reads ahead of real time. Each file starts exactly 200 ms behind live (trimmed or
padded with silence), and the phone is told that delay over A2DP delay reporting, so video on the
phone stays in sync. Both come from `target_latency_ms`. The radio adds its own buffering on top,
which btdisk can't see.

Two crystals 100 ppm apart drift 0.36 s an hour, which would eventually eat that cushion. There
is no audio PLL to tune, because the DAC is in the radio. Instead, btdisk measures the phone's
clock from the A2DP RTP timestamps and the radio's from how fast it drains the queue, then
resamples by the difference with a 48-tap polyphase ASRC (about 80 dB SNR, ~5% of the Zero W's
core). A radio whose buffer is bigger than the cushion (likely at 200 ms) waits on btdisk and hides its
clock, so there the phone is held to the Pi's clock instead. Lost Bluetooth packets are filled to their
length, so the timeline stays exact (except with aptX, whose packets carry no RTP header to tell).

The code in [`app/src`](app/src) follows the data: `a2dp` (sink endpoints on BlueZ, decoded by
`codecs`: libsbc, fdk-aac, libfreeaptx) →
`stream` (live PCM/MP3 chunks, resampled by `drift`) → `livefile` (file offsets ↔ stream) →
`player` (button logic, tags) → `fat` → `scsi` → `ffs` (USB via FunctionFS). `bluez` handles
AVRCP, pairing and reconnects.

The stick boots straight into these programs, without systemd: `btdisk-init` mounts what is
needed, sets up the USB gadget and starts btdisk, then D-Bus and bluetoothd, and restarts any
of them that exits. On the bench (systemd image before it: about 22 s) the
phone connected about 8 s after power-on. The kernel is cut down for this too (LZ4, no
tracing, security modules, netfilter or IPv6). `make image DEV=1` keeps systemd, with Wi-Fi
and ssh.

## Develop

```sh
make test          # unit tests + a real-time simulated radio (with drifting clocks)
make check-image   # the virtual disk through fsck.fat and 7-Zip
make cross         # build for the Pi with the image's SDK
make deploy        # push to a running DEV stick
make bench-test    # end to end on a bench Pi: phone → btdisk → USB → "radio"
```

`make bench-test` needs no car. The Pi's own kernel plays the radio through `dummy_hcd`, and
the phone can be another bench board, your real phone, a scripted fake on the PC, or a test
tone (`PHONE=bench|real|fake|tone`). Results land in `.bench/`.
