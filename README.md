# 2022-6 Sync Test

Two command-line tools for measuring video delay and lip sync through anything
that carries SMPTE ST 2022-6/-7 or SDI:

- **`sender`** sends a test signal as ST 2022-6, as ST 2022-7 over
  two paths, or as SDI from a Blackmagic DeckLink.
- **`probe`** receives it from ST 2022-6/-7 or a Blackmagic DeckLink
  SDI input. It reports how late each frame arrives compared with a reference
  point, and how far the audio is from the video.

Connect the sender straight to the probe to check the pair: the delay is the
network path, and lip sync is 0 ms. Then put a system in between and measure
it.

## What the sender sends

**Picture:** every raster the format table supports (1080i/p, 720p, 625i and
525i):

- GStreamer's moving ball as the background, one step per frame.
- The title, the burnt-in timecode (`HH:MM:SS:FF`, or `;FF` for drop-frame),
  the frame number, and a line describing the signal.
- A machine-readable frame marker in the bottom-left corner. It carries a run
  tag, the frame number and the time the frame was scheduled to leave, and it
  can still be read after the picture has been scaled.

**Sync pulse:** every `--flash-every` seconds (default 2) the whole picture goes
white for `--flash-frames` frames (default 1). The tone is muted for exactly
those frames. The timecode and marker are drawn over the flash, so the flash
frame can still be identified.

**Audio:** a 1 kHz tone in every channel of `--audio-groups` groups (four
channels each; the default of 4 gives all 16 channels).

- **Levels:** a staircase. Channel 1 is at `--tone-level`, default -18 dBFS, and
  each later channel is `--tone-step` lower, default 3 dB, down to -63 dBFS on
  channel 16. Every channel has its own level, so any swap, sum or missing
  channel shows on a meter.

- HD uses SMPTE ST 299-1 in the colour-difference HANC, 24-bit.
- SD uses ST 272, 20-bit.
- Channel status is professional, 48 kHz, with its CRC.

Samples are placed on the video line whose time period contains them. The audio
a frame carries is therefore exactly the audio that belongs to that frame, and a
muted frame and a flashed frame are the same frame on the wire.

**Timecode** starts at the local time of day at which frame 0 is scheduled to
leave. `--tc-start HH:MM:SS:FF` overrides it. Timecode is derived from the frame
number, never read from a clock, so frame N always shows the same timecode.

**Wire format:** each frame starts at line 1's EAV, and each line is carried as
`[EAV][LN][CRC][HANC][SAV][active]`. The EAV and SAV carry the same line's F/V
flags, and the ST 292 CRC covers the preceding active picture plus the EAV and
LN words. 525-line SD uses the SMPTE 125M field edges, bottom field first.

## Build

Linux, x86-64:

```bash
sudo apt install build-essential pkg-config \
    libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev gstreamer1.0-plugins-base
make -j
make test          # composes, sends and receives in memory; no network needed
sudo make install  # optional, to /usr/local/bin
```

For DeckLink output and capture, see [Blackmagic DeckLink](#blackmagic-decklink).

On another machine the binaries need only the GStreamer runtime
(`libgstreamer1.0-0` and `gstreamer1.0-plugins-base`); the C++ runtime is
linked in.

The raster, HBRMT, pacing, multicast and NMOS code comes from PCAP Replay 1.8.1;
see `third_party/pcapreplay/VENDORED.md`.

For line-rate pacing, allow real-time priority and a full send buffer:

```bash
sudo setcap cap_sys_nice=eip build/bin/sender
sudo sysctl -w net.core.wmem_max=8388608
```

## Sending

```bash
# which NIC?
sender --interfaces

# 1080i50, ST 2022-7 on two NICs
sender --format 1080i50 \
    --iface eth1 --group 239.10.1.1 \
    --iface-b eth2 --group-b 239.10.2.1 --port 40000

# the same, registered with NMOS so a controller can route it
sender --format 1080i50 --iface eth1 --group 239.10.1.1 \
    --iface-b eth2 --group-b 239.10.2.1 --nmos --nmos-iface eth0 \
    --label "Test signal 1080i50"

# 1080p50, flash every second, two groups of audio
sender --format 1080p50 --flash-every 1 --audio-groups 2 \
    --iface eth1 --group 239.10.1.1
```

`--formats` lists the rasters. Interlaced formats accept either rate spelling
(`1080i50` or `1080i25`).

### SDI output

```bash
# the same test signal out of DeckLink device 0 as 1080i50 SDI
sender --format 1080i50 --sdi 0
```

`--sdi N` sends to DeckLink device N instead of the network.

- **Signal:** the picture, tone, flash and mute are the same as on the network.
  The tone goes out on 8 channels, or 16 with `--audio-groups` above 2.
- **A/V timing:** each frame's picture and its audio samples carry matching
  timestamps, so the flash and the mute leave the card together.
- **Clocking:** the card's clock paces the output; there is no software pacer on
  this path.
- **Modes:** every format has a DeckLink mode except 1080PsF25.
- **NMOS:** `--sdi` cannot be combined with `--nmos`.

A console line every two seconds reports:

- the timecode on air;
- packets per second against target;
- the rate on each leg;
- repeated frames;
- the worst pacer lateness.

**Repeated frames** means picture composition fell behind line rate and a frame
was sent twice rather than leaving a gap. It should stay at zero. A repeat shifts
every later frame's marker time by one frame.

**NMOS node IDs** are derived from the machine and the node port. Restart on the
same `--nmos-port` to keep a controller's existing route. If that port is still
held, the node moves to the next free one and its sender gets a new ID.

## Receiving: probe

Run it on a machine that can see the points you want to compare. It stamps
every frame's arrival on that machine's monotonic clock, so the sender's clock
does not enter into it.

```bash
# lip sync of the sender alone, received directly
probe --source tx=st2022:239.10.1.1@eth1,239.10.2.1@eth2

# delay through a system: its ST 2022-6 input against its SDI output on a DeckLink
probe --source in=st2022:239.10.1.1@eth1,239.10.2.1@eth2 \
                  --source out=sdi:0 --offset out=2 \
                  --interval 5 --csv delay.csv
```

**Sources:**

- `st2022:GROUP[:PORT][@IFACE][,GROUP[:PORT][@IFACE]]` is one leg, or two for
  ST 2022-7.
- `sdi:DEVICE[:MODE]` is a DeckLink input, with its mode detected automatically
  by default.
- The first source is the reference unless `--ref` names another.

**Delay** is a frame's arrival at a source, minus the arrival of the same frame
number at the reference, minus that source's `--offset` in its own frames. A
DeckLink reports 2 frames of its own capture delay.

- **Frame matching:** on an interlaced-to-progressive path a frame number
  appears on two output frames, and the first one counts.
- **Cross-check:** the flash-to-flash delay is printed as well. It still works
  when no marker survives.

**Lip sync** is a source's tone mute minus its flash, measured on that one
source, so it needs no reference. It is reported for every source whose audio
carries the tone: embedded ST 299/272 on ST 2022-6, and the DeckLink's first
channel on SDI. Positive means the audio is late. Mutes are timed to the sample.

**Where the picture is:** a source may show the test picture scaled inside a
tile. On the first flash the probe takes the rectangle that jumps from dark to
white as the picture and reads the marker from its scaled position there.
`--region NAME=X,Y,W,H` sets the rectangle by hand. Frames before the first
flash count as "no marker".

**Receive sockets** are bound to the group address with `IP_MULTICAST_ALL`
off, and the merge locks to one RTP SSRC. This matters on a host where other
receivers use the same port: Linux otherwise hands every joined group to every
socket on that port.

## Blackmagic DeckLink

Neither tool contains, links or ships any Blackmagic code or SDK. DeckLink
output (`sender --sdi`) and capture (`probe`
`sdi:` sources) both go through GStreamer's `decklink` plugin. That plugin
loads Blackmagic's driver library while it runs.

**What to install:**

1. **Blackmagic Desktop Video**, from Blackmagic Design's support site. It
   installs the DeckLink kernel driver and the runtime library
   `libDeckLinkAPI.so`; on Debian and Ubuntu the package is `desktopvideo`,
   which puts the library at `/usr/lib/libDeckLinkAPI.so`.
2. **GStreamer's `decklink` plugin**, in `gstreamer1.0-plugins-bad` on Debian
   and Ubuntu.

**Where the library is looked for:** the plugin loads `libDeckLinkAPI.so` by
name when a DeckLink element starts, not when the tools start. The system's
normal library search applies:

- `LD_LIBRARY_PATH`;
- the `ld.so` cache (`/etc/ld.so.conf`, refreshed with `ldconfig`);
- `/lib` and `/usr/lib`.

If Desktop Video put the library somewhere else, add that directory to
`LD_LIBRARY_PATH` or to `/etc/ld.so.conf.d/` and run `ldconfig`.

**Checking the installation:**

```bash
ls -l /usr/lib/libDeckLinkAPI.so          # or wherever Desktop Video put it
gst-inspect-1.0 decklinkvideosink         # the plugin is present
gst-inspect-1.0 decklinkvideosrc
```

Without the library, the network sender and receiver still work; only `--sdi`
and `sdi:` sources fail to start, and they say why.

A DeckLink can drive an output and capture an input at the same time, so a
card with both connectors, looped back, lets the sender and the probe check
each other on one machine.

## Limits and open points

- **ST 299-1 ECC** is BCH(31,25) per bit plane, with generator
  x^6+x^5+x^3+x^2+x+1. It has not yet been confirmed against third-party kit. A
  receiver that enforces ECC and rejects the audio points here first.
- **No audio control packets** are sent (ST 299-1 DID E3..E0 / ST 272 extended
  packets). Receivers assume 48 kHz synchronous audio without them.
- **Timing is internal:** the sender is not PTP-locked. The HBRMT R field is 3.
- **Interlaced motion:** interlaced frames carry the same picture in both fields,
  so the ball moves once per frame, not once per field.
- **SD markers over SDI:** at 625 and 525 lines each marker cell is only 2
  pixels. SDI's chroma filtering often blurs it past decoding: a DeckLink
  loopback read the marker on 23 of 296 625i50 frames. Flash-to-flash delay and
  lip sync are unaffected, and HD markers decode on every frame.
- **The marker time** is when the frame was due to leave the sending machine,
  by its clock. The probe does not use it; its delays come from its own
  arrival stamps.

## Licence

MIT, see `LICENSE`.
