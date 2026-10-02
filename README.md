# Synth-Mvave_SMK25II
A collection of custom C++, Raspberry Pi, desktop and Android projects designed to unlock the full potential of the M-VAVE SMK25II through custom MIDI handling, synthesis, sampling, looping, audio recording and personalized controls.


# M-VAVE SMK25II Custom Projects

Custom software projects for the M-VAVE SMK25II MIDI controller on Raspberry Pi, desktop computers and Android devices.

The goal of this repository is to explore the M-VAVE SMK25II beyond its standard factory functions and use it as a flexible platform for custom MIDI, synthesis, sampling, looping and experimental audio applications.

This repository will contain different projects and experiments for:

- Raspberry Pi
- Windows desktop computers
- Android devices

The idea is to create custom software that allows the M-VAVE SMK25II to be used in more flexible and personalized ways.

---

# First Project: Raspberry Pi C++ Synth / Sampler / Looper

The first project published in this repository is a C++ application designed to run on Raspberry Pi and communicate with the M-VAVE SMK25II through MIDI.

The first version of the program was written with the help of AI.

At first glance this may make the project appear simple, but during development many parts of the code were reviewed, tested and modified several times.

A considerable amount of work was required to eliminate bugs and improve the behavior of:

- MIDI event handling
- Polyphonic synthesis
- WAV sample playback
- Sample mapping
- Sample start position
- Play / pause / resume behavior
- Looper recording
- Loop playback
- Multiple loop tracks
- Audio mixing
- 8-bit sound effects
- Volume management
- Audio recording
- Bluetooth MIDI detection
- Raspberry Pi audio output
- Sample playback logic
- Long WAV playback
- Recording samples already in progress
- Custom control behavior

Many parts of the program were rewritten or adjusted multiple times in order to make the looper, WAV sampler and MIDI behavior more reliable.

The project has therefore become an experimental platform that continues to evolve.

---

# Project Goal

The goal is to get the maximum possible flexibility from the M-VAVE SMK25II by creating custom functions that are not necessarily available in the original controller software.

Instead of using the keyboard only as a normal MIDI controller, the Raspberry Pi can turn it into a standalone:

- Synthesizer
- WAV sampler
- Drum machine
- MIDI looper
- Experimental 8-bit instrument
- Performance controller
- Audio recorder
- Custom live-performance system

The idea is to continue experimenting with different hardware and software platforms.

---

# Current Raspberry Pi Version

The current C++ version includes:

- 16 internal synthesized instruments
- Dedicated WAV Sample Kit mode
- WAV samples loaded into memory
- Custom `map.txt` sample mapping
- WAV playback directly from MIDI keys
- Gate sample mode
- One-shot sample mode
- Loop sample mode
- Individual sample volume
- Stereo WAV playback
- 7 independent MIDI loop tracks
- Recording of synth notes
- Recording of WAV sample events
- Sample-position recording
- Play / pause / resume for long WAV samples
- Double press to stop and reset one-shot samples
- Full stereo mix recording to WAV
- Automatic M-VAVE SMK25II MIDI detection
- USB audio output
- HDMI audio fallback
- Terminal instrument selection
- Per-instrument volume control
- Experimental 8-bit synthesis
- Multiple percussion instruments
- Custom MIDI mappings
- Bluetooth MIDI support through ALSA MIDI

---

# Internal Synthesizer

The program currently contains 16 internally generated instruments.

These sounds are produced directly by the C++ audio engine without using prerecorded WAV samples.

The internal instruments include different waveform and percussion experiments such as:

- Square wave
- Saw synth
- Triangle
- Soft sine
- Organ
- Bass
- Lead
- Pluck
- Bell
- Kick
- Snare
- Closed hi-hat
- Open hi-hat
- Tom
- Clap
- Drum Kit

The internal synth section also includes experimental retro and 8-bit sound characteristics.

---

# 8-bit Synthesis

The internal synthesizer contains several generated instruments and experimental 8-bit / retro sounds.

These sounds are generated directly by the C++ application rather than being played from prerecorded samples.

This makes it possible to experiment with:

- Waveforms
- Envelopes
- Noise
- Pitch changes
- Percussion synthesis
- Retro digital audio characteristics
- 8-bit style effects

The WAV sampler follows a separate clean audio path so normal WAV samples can be reproduced without the intentional 8-bit degradation used by some synthesized instruments.

---

# WAV Sample Kit - Instrument 18

Instrument `18` activates the WAV Sample Kit.

Samples are configured through:

```text
samples/map.txt
```

Each MIDI note can be associated with:

- A WAV file
- Playback mode
- Individual volume

Example:

```text
24=808/kick.wav,oneshot,120
25=808/snare.wav,gate,100
26=fx/crash.wav,oneshot,100
27=voices/hello.wav,gate,90
28=loops/ambient.wav,loop,80
```

Subdirectories are supported.

For example:

```text
808/kick.wav
808/snare.wav
voices/hello.wav
fx/crash.wav
loops/ambient.wav
```

---

# Recommended WAV Format

The recommended WAV format is:

```text
PCM 16-bit
48000 Hz
Stereo
```

A compatible WAV can be created with FFmpeg:

```bash
ffmpeg -i input.wav -ar 48000 -ac 2 -c:a pcm_s16le output.wav
```

To convert all WAV files inside a directory and all its subdirectories:

```bash
find . -type f -iname "*.wav" -print0 | while IFS= read -r -d '' f; do
    tmp="${f}.tmp.wav"

    ffmpeg -y -i "$f" \
        -ar 48000 \
        -ac 2 \
        -c:a pcm_s16le \
        "$tmp"

    if [ $? -eq 0 ]; then
        mv -f "$tmp" "$f"
        echo "OK: $f"
    else
        rm -f "$tmp"
        echo "ERROR: $f"
    fi
done
```

---

# map.txt Format

The general syntax is:

```text
MIDI_NOTE=PATH_TO_WAV,MODE,VOLUME
```

Example:

```text
24=808/kick.wav,oneshot,100
```

This means:

```text
24              MIDI note
808/kick.wav    WAV file
oneshot         playback mode
100             volume
```

---

# Sample Playback Modes

## oneshot

Example:

```text
24=kick.wav,oneshot,100
```

The sample starts when the MIDI key is pressed.

Releasing the key does not immediately stop the WAV.

This mode is useful for:

- Kick drums
- Snare drums
- Cymbals
- Sound effects
- Complete phrases
- Long WAV samples

The current implementation also provides custom controls for long one-shot samples.

Behavior:

```text
First press      -> PLAY
Single press     -> PAUSE
Next press       -> RESUME
Double press     -> STOP and reset to beginning
```

After a full stop, the next press starts the WAV again from the beginning.

---

## gate

Example:

```text
25=voice.wav,gate,100
```

The WAV plays while the MIDI key is held.

When the key is released, playback stops.

Behavior:

```text
KEY DOWN -> PLAY
KEY UP   -> STOP
```

This mode is useful for:

- Voice samples
- Effects
- Pads
- Long sounds
- Samples that should follow the duration of the MIDI key press

---

## loop

Example:

```text
26=ambient.wav,loop,100
```

The sample repeats continuously while active.

This mode can be useful for:

- Rhythmic loops
- Ambient sounds
- Background layers
- Repeating effects

---

# Sample Volume

The third parameter controls the individual sample volume.

Example:

```text
24=kick.wav,oneshot,150
```

The volume range currently used is:

```text
0 - 200
```

Typical values:

```text
50   = low
80   = slightly reduced
100  = normal
120  = boosted
150  = strong boost
200  = maximum configured range
```

It is usually recommended to start around:

```text
80 - 100
```

Very high values can cause clipping when multiple samples are playing simultaneously.

---

# Complete map.txt Example

```text
# ============================================================
# M-VAVE SMK25II - WAV SAMPLE MAP
# Used by Instrument 18
# ============================================================
#
# FORMAT:
#
# MIDI_NOTE=FILE,MODE,VOLUME
#
# Example:
#
# 24=808/kick.wav,oneshot,100
#
# MODES:
# gate     = plays while key is held, stops on key release
# oneshot  = starts on press, supports play/pause/resume and double-press reset
# loop     = repeats continuously while active
#
# VOLUME:
# 50  = low
# 80  = reduced
# 100 = normal
# 120 = boosted
# 150 = strong boost
# 200 = maximum configured range
#
# RECOMMENDED WAV FORMAT:
# PCM 16-bit / 48000 Hz / Stereo
# ============================================================

12=808/kick01.wav,oneshot,100
13=808/kick02.wav,oneshot,100
14=808/snare01.wav,oneshot,100
15=808/clap01.wav,oneshot,100
16=808/hihat_closed.wav,oneshot,100
17=808/hihat_open.wav,oneshot,100
18=808/crash.wav,oneshot,100
19=808/ride.wav,oneshot,100

37=short_phrases/phrase01.wav,gate,100
38=short_phrases/phrase02.wav,gate,100
39=short_phrases/phrase03.wav,gate,100

50=fx/fx01.wav,oneshot,100
51=fx/fx02.wav,oneshot,100
52=fx/riser.wav,gate,80

60=loops/loop01.wav,loop,80
```

---

# MIDI Looper

The program provides seven independent loop tracks.

Each track can record MIDI events produced by the keyboard.

The looper works with both:

- Internal synthesized instruments
- WAV samples from Instrument 18

Multiple tracks can play simultaneously.

This makes it possible to progressively build a performance.

Example:

```text
Track 1 -> kick / drums
Track 2 -> bass
Track 3 -> chords
Track 4 -> melody
Track 5 -> effects
Track 6 -> samples
Track 7 -> additional layer
```

---

# WAV Sample Position Recording

An important function added during development is sample-position recording.

If a long WAV is already playing and loop recording begins in the middle of the sample, the program can store the current WAV playback position.

Example:

```text
WAV starts
|
wait 5 seconds
|
REC starts
|
record 3 seconds
|
REC stops
```

The loop can remember the sample position instead of simply restarting the WAV from the beginning.

This behavior required several revisions of the original looper logic.

---

# Loop Playback Behavior

Loop tracks can be started and stopped independently.

When a loop is stopped and played again, playback restarts from the beginning of the loop.

Multiple loop tracks can run together.

This allows the controller to be used as a simple multi-layer performance tool.

---

# Full Mix Recording

The complete audio output can also be recorded directly from the application.

From the terminal:

```text
r
```

starts recording.

```text
s
```

stops recording and saves the WAV.

The recording contains the final stereo mix produced by the application.

Files are stored inside the:

```text
recordings/
```

directory.

---

# Terminal Controls

Available terminal commands include:

```text
1-16       Select internal synth instrument
18         Select WAV Sample Kit
i          Show instrument list
n          Select next instrument
v N        Set current instrument volume
vol        Display current volume
status     Show synth status
r          Start stereo mix recording
s          Stop and save recording
recstatus  Show recording status
stop       Stop active notes
```

---

# MIDI Controller

The current project has been developed using:

```text
M-VAVE SMK25II
```

The application automatically searches for the SMK25II MIDI device.

The controller is currently used through Bluetooth MIDI on Raspberry Pi.

---

# Audio Output

The application can use:

- USB audio interfaces
- USB sound cards
- HDMI audio

USB audio is preferred.

If the USB output is not available, the program can fall back to HDMI.

---

# Building on Raspberry Pi

Install the required compiler and ALSA development library:

```bash
sudo apt update
sudo apt install build-essential libasound2-dev
```

Compile:

```bash
g++ main.cpp -o synth -lasound -pthread -O2 -std=c++17
```

Run:

```bash
./synth
```

---

# Suggested Directory Structure

Example project structure:

```text
mvave-synth/
|
|-- main.cpp
|-- synth
|
|-- samples/
|   |
|   |-- map.txt
|   |
|   |-- 808/
|   |   |-- kick.wav
|   |   |-- snare.wav
|   |   |-- hihat.wav
|   |
|   |-- short_phrases/
|   |   |-- phrase01.wav
|   |   |-- phrase02.wav
|   |
|   |-- fx/
|   |   |-- crash.wav
|   |   |-- riser.wav
|   |
|   |-- loops/
|       |-- loop01.wav
|
|-- recordings/
```

---

# Hardware Used During Development

The current project has been developed and tested using:

- Raspberry Pi
- M-VAVE SMK25II MIDI controller
- Bluetooth MIDI
- USB audio interface / USB sound card
- Amplified speakers or headphones

---

# Demo Video

A first demonstration of the Raspberry Pi version running with the M-VAVE SMK25II is available here:

**YouTube demonstration:**

VIDEO_LINK_HERE

The video shows the project running on real hardware and demonstrates some of the custom synth, sampling, looping and MIDI functions.

---

# Future Development

This repository will not be limited to the Raspberry Pi version.

More projects and experimental versions are planned.

---

# Windows Desktop Version

A desktop C++ version is planned for Windows computers.

The goal is to use the additional processing power and audio interfaces available on desktop computers while keeping the custom M-VAVE workflow.

Possible features include:

- Low-latency MIDI
- Desktop audio interfaces
- WASAPI / ASIO audio
- Custom synthesizers
- WAV sampling
- Multi-track looping
- Advanced audio effects
- Larger sample libraries
- Performance tools
- Custom MIDI mappings
- Additional real-time controls

The goal is to maintain a similar workflow between Raspberry Pi and desktop versions.

---

# Android Version

An Android application is also planned.

The goal is to experiment with using an Android phone or tablet together with the M-VAVE SMK25II for portable MIDI control and custom functions.

Possible areas of experimentation include:

- BLE MIDI
- Custom MIDI controls
- MIDI monitoring
- Synth control
- Sample triggering
- Mobile performance interfaces
- Custom mappings
- Loop control
- Integration with the Raspberry Pi project
- Integration with future desktop versions

---

# Cross-Platform Idea

The long-term goal is to experiment with the same M-VAVE SMK25II controller across multiple platforms:

```text
M-VAVE SMK25II
      |
      +---- Raspberry Pi
      |
      +---- Windows Desktop
      |
      +---- Android
```

Each platform can provide different possibilities while maintaining a similar custom MIDI workflow.

---

# Development Philosophy

This repository is primarily an experimental project.

The objective is not only to create a finished synthesizer, but also to explore how inexpensive hardware, embedded computers, desktop systems, Android devices, MIDI controllers and custom software can be combined to create new musical instruments and performance tools.

AI-assisted development is part of the experimentation process.

The first version of the Raspberry Pi software was written with the help of AI, but many parts of the code were reviewed, tested and modified repeatedly on real hardware.

Several problems only became visible during real use, especially in areas such as:

- Looper timing
- WAV sample behavior
- MIDI NOTE ON / NOTE OFF handling
- Audio mixing
- Sample start position
- Play / pause logic
- Recording
- 8-bit audio effects
- Volume handling
- Bluetooth MIDI behavior

For this reason, development has involved repeated testing, debugging and modification rather than simply generating the program once.

The project will continue to evolve as new ideas, functions, platforms and experiments are added.

---

# Current Status

The Raspberry Pi version is already functional and can be used for:

- Synthesizer performance
- WAV sample triggering
- Drum samples
- Voice samples
- Loop building
- Experimental 8-bit sounds
- MIDI performance
- Stereo audio recording

New versions and new projects will be added over time.

---

# Platforms

Current:

```text
Raspberry Pi
```

Planned:

```text
Windows Desktop
Android
```

More experimental platforms may be added in the future.

---

# License

Choose and add the license that best fits your project.

For example:

```text
MIT License
```

or another license depending on how you want the code to be reused.

---

# Notes

This project is independent and experimental.

M-VAVE and SMK25II are product names belonging to their respective owners.

This repository is not an official M-VAVE project.

