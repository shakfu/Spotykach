// SYNTHUX ACADEMY /////////////////////////////////////////
// SPOTYKACH ///////////////////////////////////////////////
#pragma once

// Build-time engine selection (item 3b-1). The Makefile's ENGINE variable emits one
// -DSPK_ENGINE_* define; this header maps it to the concrete `ActiveEngine` type that app.cpp
// instantiates as a static value member. The platform (CoreUI/Storage) only ever sees IEngine,
// so this is the single place the firmware names a concrete engine. See docs/architecture.md.

#if defined(SPK_ENGINE_GRANULAR)
  #include "engine/granular/granular_engine.h"
  namespace spotykach { using ActiveEngine = GranularEngine; }
#elif defined(SPK_ENGINE_PASSTHROUGH)
  #include "engine/passthrough/passthrough_engine.h"
  namespace spotykach { using ActiveEngine = PassthroughEngine; }
#elif defined(SPK_ENGINE_DELAY)
  #include "engine/delay/delay_engine.h"
  namespace spotykach { using ActiveEngine = DelayEngine; }
#elif defined(SPK_ENGINE_QDELAY)
  #include "engine/qdelay/qdelay_engine.h"
  namespace spotykach { using ActiveEngine = QdelayEngine; }
#elif defined(SPK_ENGINE_EDRUMS)
  #include "engine/edrums/edrums_engine.h"
  namespace spotykach { using ActiveEngine = EdrumsEngine; }
#elif defined(SPK_ENGINE_RESO)
  #include "engine/reso/reso_engine.h"
  namespace spotykach { using ActiveEngine = ResoEngine; }
#elif defined(SPK_ENGINE_MOSC)
  #include "engine/mosc/mosc_engine.h"
  namespace spotykach { using ActiveEngine = MoscEngine; }
#elif defined(SPK_ENGINE_GRAINCLOUD)
  #include "engine/graincloud/graincloud_engine.h"
  namespace spotykach { using ActiveEngine = GraincloudEngine; }
#elif defined(SPK_ENGINE_TAPE)
  #include "engine/tape/tape_engine.h"
  namespace spotykach { using ActiveEngine = TapeEngine; }
#elif defined(SPK_ENGINE_REVERB)
  #include "engine/reverb/reverb_engine.h"
  namespace spotykach { using ActiveEngine = ReverbEngine; }
#elif defined(SPK_ENGINE_SHUTTLE)
  #include "engine/shuttle/shuttle_engine.h"
  namespace spotykach { using ActiveEngine = ShuttleEngine; }
#elif defined(SPK_ENGINE_SOFTCUT)
  #include "engine/softcut/softcut_engine.h"
  namespace spotykach { using ActiveEngine = SoftcutEngine; }
#elif defined(SPK_ENGINE_RADIO)
  #include "engine/radio/radio_engine.h"
  namespace spotykach { using ActiveEngine = RadioEngine; }
#elif defined(SPK_ENGINE_BARD)
  #include "engine/bard/bard_engine.h"
  namespace spotykach { using ActiveEngine = BardEngine; }
#elif defined(SPK_ENGINE_GLITCH)
  #include "engine/glitch/glitch_engine.h"
  namespace spotykach { using ActiveEngine = GlitchEngine; }
#elif defined(SPK_ENGINE_PSTRETCH)
  #include "engine/pstretch/pstretch_engine.h"
  namespace spotykach { using ActiveEngine = PstretchEngine; }
// gen~ engines (SPK_ENGINE_GEN_*) are appended below by scripts/gen_engine.py.
// >>> gen:gigaverb >>>
#elif defined(SPK_ENGINE_GIGAVERB)
  #include "engine/gigaverb/gigaverb_engine.h"
  namespace spotykach { using ActiveEngine = GigaverbEngine; }
// <<< gen:gigaverb <<<
// >>> faust:chorus >>>
#elif defined(SPK_ENGINE_CHORUS)
  #include "engine/chorus/chorus_engine.h"
  namespace spotykach { using ActiveEngine = ChorusEngine; }
// <<< faust:chorus <<<
// >>> faust:filter >>>
#elif defined(SPK_ENGINE_FILTER)
  #include "engine/filter/filter_engine.h"
  namespace spotykach { using ActiveEngine = FilterEngine; }
// <<< faust:filter <<<
// >>> faust:voice >>>
#elif defined(SPK_ENGINE_VOICE)
  #include "engine/voice/voice_engine.h"
  namespace spotykach { using ActiveEngine = VoiceEngine; }
// <<< faust:voice <<<
#elif defined(SPK_ENGINE_CSOUND)
  // QSPI-only target (links libcsound.a, BOOT_QSPI) - NOT an SRAM engine. See docs/dev/csound-impl.md.
  #include "engine/csound/csound_engine.h"
  namespace spotykach { using ActiveEngine = CsoundEngine; }
#elif defined(SPK_ENGINE_CHUCK)
  // QSPI-only target (links libchuck.a, BOOT_QSPI) - NOT an SRAM engine. See docs/dev/chuck-impl.md.
  #include "engine/chuck/chuck_engine.h"
  namespace spotykach { using ActiveEngine = ChuckEngine; }
#else
  #error "No engine selected: build with ENGINE=granular (default), passthrough, delay, edrums, reso, graincloud, tape, reverb, shuttle, softcut, radio, bard, or chorus"
#endif
