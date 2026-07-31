 [sk-engines 0.6.0 release](https://github.com/shakfu/sk-engines/releases/tag/0.6.0):

- bard: bookmark-navigated audiobook decks — each deck reads a spoken-word recording from the SD card and jumps book-to-book or bookmark-to-bookmark, with varispeed, pitch-preserved playback, colour and a room
- chuck: the ChucK language + VM as a synth — a .ck program defines the sound, with strongly-timed concurrent voices; load and switch programs live from the SD card
- csound: a full Csound 7 instance as a synth — a .csd orchestra defines the sound; load orchestras from the SD card, switch them live, and play them over MIDI
- delay: tempo-synced stereo delay with Clean / Tape / Shimmer characters, Stereo / DoubleMono / Ping-pong topologies, feedback tone, a modulation LFO, and Freeze / Reverse pads
- edrums: four-voice Euclidean drum machine with synthesized drums you shape live — per-drum gain, decay, and grit/flux macros for drive, pitch-sweep, brightness and body↔noise balance
- filter: dual resonant filter, one independent voice per channel — the parallel dual-deck Faust demo
- glitch: dual-deck lo-fi / circuit-bent noise voice — 12 curated algorithms ported from Rob Scape's Noisferatu: bit-mangling, logic noise, generative blips and rhythmic noise, aliased and crunchy by design
- graincloud: a polyphonic grain cloud — the granular looper with its grain core replaced by a GrainflowLib cloud, scattering dozens of independently pitched and panned grains over the recorded buffer
- mosc: dual macro-oscillator giving each deck a full 24-engine Mutable Instruments Plaits voice — virtual analog, FM, wavetable, granular, additive, chord, speech, modal, drums and more
- pstretch: real-time PaulStretch ambient time-smear — huge overlapping FFT windows with randomized phases turn any input into a diffuse, endlessly evolving wash
- qdelay: a dub/ambient flavor of the delay with a Clean / Diffuse / Duck character palette — Diffuse runs the feedback through an 8-stage allpass diffuser for a reverb-like wash, Duck ducks the repeats under the dry input so they bloom in the gaps
- radio: dual virtual RadioMusic — two independent radios over a shared SD library of banks, with the signature free-running virtual playhead, so every station keeps broadcasting while you're tuned elsewhere
- reso: resonator / plucked-string instrument on the Mutable Instruments Rings DSP — modal, sympathetic-string, string, FM and string+reverb models, with discrete plucks, a live-input resonator, or a scatter cloud
- reverb: route-aware stereo reverb with three all-Faust algorithms — a Dattorro plate, a Zita-rev1 hall and a Greyhole — selectable live, with an independent mono plate per deck in DoubleMono
- shuttle: buffer-based bipolar/reverse varispeed tape — four in-RAM tracks with PITCH as a capstan-speed knob: noon stops, clockwise runs forward, counter-clockwise runs in reverse
- softcut: dual-deck crossfaded overdub looper on monome's softcut-lib — plays and records the same loop at once with click-free subsample crossfades and sound-on-sound layering; the first take defines the loop length
- tape: dual streaming tape deck — two independent record/playback decks streamed straight to and from the SD card, with no in-memory length cap
- voice: drone oscillator into a resonant filter — the series (chain) dual-deck Faust demo
