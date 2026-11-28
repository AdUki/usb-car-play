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

Plug the Pi's **USB** port (not PWR) into the radio. Within 5 minutes of power-on, pair your
phone with **Car USB**. No PIN is needed.
After power-on it calls the phone that was connected last for the first 10 s (`autoconnect_s`),
until a phone connects. After that it calls no phone again until the next
power-on: disconnect it on the phone and it stays disconnected.

### Settings

Put the SD card into any PC and open its `boot` drive: `btdisk.txt` there has every setting,
each with what it does (MP3 or WAV, the latency, autoplay, pausing, the tags, the codecs, …).
Edit it in any text editor, save, and put the card back: the settings apply at the next
power-on, no new image needed. Delete a line and that setting goes back to how the image was
built (`/etc/btdisk.conf`, made from [`config/btdisk.conf`](config/btdisk.conf)). A typo can't
stop the stick: a line or value btdisk can't read keeps its default, and the log says which.

The image writes to the SD card only when a phone pairs or a different one connects (BlueZ's
pairings and the last phone, on the data partition): the log stays in RAM. To find out what
goes wrong in the car, set `trace = true` and `file = /data/btdisk.log` in `btdisk.txt` (or
build with `make image TRACE=1`): then the data partition keeps the story across power cuts. `btdisk.log` (and `.1`) has what the radio read and when it paused,
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
which btdisk can't see. A read is answered only once all of it is live: radios read a few KiB at a
time, but a PC asks for up to 128 KiB (4 s of MP3), which would reach its player in lumps. A read
bigger than the cushion makes btdisk raise it to cover such reads (about 5 s for MP3, 2 s for
WAV, `max_latency_ms`) and report the new delay to the phone.

Two crystals 100 ppm apart drift 0.36 s an hour, which would eventually eat that cushion. There
is no audio PLL to tune, because the DAC is in the radio. Instead, btdisk measures the phone's
clock from the A2DP RTP timestamps and the radio's from how fast it drains the queue, then
resamples by the difference with a 48-tap polyphase ASRC (about 80 dB SNR, ~5% of the Zero W's
core). A radio whose buffer is bigger than the cushion (likely at 200 ms) waits on btdisk and hides its
clock, so there the phone is held to the Pi's clock instead. Lost Bluetooth packets are filled to their
length, so the timeline stays exact (except with aptX, whose packets carry no RTP header to tell).

### The code

The code in [`app/src`](app/src) is four boxes, and `main.cpp` builds them and wires them
together. Each box has one way in:

```
        PHONE                                                      RADIO
          ║ A2DP, AVRCP                                        USB ║
   ┌──────╨──────┐                                         ┌───────╨──────┐
   │  Bluetooth  │◀───────────── MediaControl ─────────┐   │   UsbDisk    │
   └──────┬──────┘   command(), track(), set_delay()   │   └──┬────────▲──┘
          │                                            │      │        │
          │ PCM                                        │      │ reads, │ file
          │                                            │      │ mount, │ bytes
   ┌──────▼──────┐             chunks              ┌───┴──────▼──┐     │
   │ LiveStream  │────────────────────────────────▶│   Player    │─────┘
   │             │◀──────── mute, unmute ──────────│             │
   └─────────────┘                                 └─────────────┘
```

Inside the boxes (a nested item is owned by the one above it, *in italics* a thread of its own):

- **main.cpp** reads the config (`/etc/btdisk.conf`, then `/boot/btdisk.txt` on the SD card,
  editable on a PC) with `read_settings()` into Settings, then builds the four boxes and wires them.
  - *tick thread*: `Player::tick()` once a second, plus the trace heartbeat
- **Bluetooth**: Bluez, which implements MediaControl. *bus thread*: AVRCP, the pairing agent, reconnects
  - A2dpSink. *reader thread*: one `MediaEndpoint1` per codec
    - Decoder: SbcDecoder, AacDecoder or AptxDecoder
- **LiveStream**: `write_pcm()` → Resampler → mute gate → Encoder → ChunkQueue
  - PtsClock: the phone's clock, from the RTP timestamps
  - RadioClock: the radio's clock, from the queue's read stats
  - Encoder: WavEncoder or Mp3Encoder (lame)
  - ChunkQueue
  - *clock thread*: silence while the phone sends nothing
- **Player**. *watch thread*: unmutes after Next/Previous
  - LiveFileData: the file being played ↔ the live queue
- **UsbDisk**. *main thread*: ep0, binds the USB controller
  - BulkOnly over EpTransport. *bot thread*
    - ScsiDisk
      - FatVolume: calls VirtualFile's read / peek / long_name, which land in the Player

Every layer under UsbDisk knows only the one below it. FatVolume reaches the Player only
through the callbacks in each VirtualFile, so the storage side never includes a player header.

The files, box by box: Bluetooth is `bluez`, `a2dp` and `codecs` (libsbc, fdk-aac,
libfreeaptx). LiveStream is `stream` and `drift`. Player is `player`, `livefile` and `formats`
(the WAV, ID3 and MP3 headers). UsbDisk is `usb_disk`, `scsi` and `fat`. The settings are
`config` and `settings`.

### Booting

The image holds only what the stick needs, and the path from power to music is cut short. It
boots in about 4 s, and the phone is connected about 5 s after power-on.

- **Firmware straight to the kernel**: no U-Boot, no initramfs, no splash, no boot delay, and the
  CPU in turbo mode for the first seconds.
- **A small kernel**: LZ4-compressed, which unpacks several times faster than gzip on the ARM11.
  The USB gadget and the Bluetooth core are built in. Sound, graphics, tracing, security modules,
  netfilter, IPv6 and SCSI are left out.
- **No init system**: `btdisk-init` is PID 1. It loads the Bluetooth driver in the background,
  sets up the USB gadget and starts btdisk first, so the radio sees the stick while the rest
  starts. Then come D-Bus and bluetoothd, and any of them that exits is restarted.
- **Four programs**: btdisk, D-Bus, bluetoothd (Classic only, without the plugins for profiles a
  car stick never offers) and a login on the serial console. No audio server, no network, and
  Wi-Fi is off.
- **Read-only**: the root is mounted read-only, the pairings live on a small `/data` partition
  and the log in RAM, so cutting the power is always safe.

`make image DEV=1` builds a development image instead, with systemd, Wi-Fi and ssh.

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
