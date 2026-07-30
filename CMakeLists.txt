# SPIKE (TODO P3): evaluate converting the firmware build to CMake.
# Goal is parity with the proven Make build, NOT idiomatic-CMake defaults.
# The Make build compiles app TUs at -O2/no-LTO/c++17, defines BOOT_APP, and links
# the custom alt_sram.lds for the SRAM bootloader. Each of those is reproduced below;
# the comments flag every place where DaisyProject.cmake's defaults would diverge.

cmake_minimum_required(VERSION 3.20)
cmake_policy(SET CMP0048 NEW)
cmake_policy(SET CMP0135 NEW)

# Autodetect the arm-none-eabi toolchain before project() (mirrors libDaisy's own CMakeLists).
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    include(${CMAKE_CURRENT_LIST_DIR}/lib/libDaisy/cmake/toolchains/autodetect.cmake)
endif()

project(SK_ENGINES VERSION 0.3.2 LANGUAGES C CXX ASM)

# PARITY (opt levels): the Make build is deliberately split -- libDaisy and DaisySP compile at -O3,
# the app at -O2 -- and uses no LTO. DaisyProject forces -O3+LTO on Release and -Og on Debug; neither
# matches. Debug is the only config that does NOT add -flto, so we build Debug and steer opt levels by
# hand below. NDEBUG is also defined only on Release/MinSizeRel and Make never defines it, so Debug
# matches there too.
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "" FORCE)
endif()

# PARITY (libs at -O3): set -O3 as a directory option BEFORE add_subdirectory so every libDaisy and
# DaisySP target inherits it at creation (directory COMPILE_OPTIONS propagate to subdirs added after
# the call). The app target, created later by DaisyProject, also inherits this -O3 but is overridden
# back to -O2 after include(DaisyProject) -- a later target-level -O2 wins on the command line.
add_compile_options($<$<CONFIG:Debug>:-O3>)
# Match DaisyProject's own daisy_targets() opt push (applied to the daisy lib) to -O3 as well, so it
# does not fight the directory -O3 with a -O2.
set(FIRMWARE_DEBUG_OPT_LEVEL -O3)

# libDaisy + DaisySP as subprojects. add_subdirectory(libDaisy) also pushes the cmake/ dir
# onto CMAKE_MODULE_PATH so `include(DaisyProject)` resolves below.
add_subdirectory(lib/libDaisy)
add_subdirectory(lib/DaisySP)
# PARITY: Make builds DaisySP at -std=gnu++14; its CMake target otherwise picks up gnu++17.
set_target_properties(DaisySP PROPERTIES CXX_STANDARD 14)

# --- Engine selection (mirrors the ENGINE switch in the Makefile; default = granular) ---
if(NOT DEFINED ENGINE)
    set(ENGINE granular)
endif()
# Per-engine extras (defaults; the reso branch overrides them for its vendored Rings/stmlib DSP).
set(ENGINE_INCLUDES "")            # extra -I roots for the firmware target
set(ENGINE_DEFINES  "")            # extra compile definitions
set(ENGINE_OPTIONS  "")            # extra compile options (gigaverb suppresses gen~ unused-* warnings)
set(APP_OPT -O2)                   # app-TU opt level (overrides the inherited -O3); reso/mosc drop to -Os
set(ENGINE_LDSCRIPT alt_sram.lds)  # SRAM engines (default); the QSPI engines (mosc/csound/chuck) override
set(ENGINE_LIBS "")                # external static libs to link (csound/chuck: libcsound.a / libchuck.a)
set(ENGINE_LINK_OPTIONS "")        # extra link options (QSPI: -u _printf_float + the SDRAM malloc wraps)
if(ENGINE STREQUAL granular)
    set(ENGINE_DEFINE SPK_ENGINE_GRANULAR)
    file(GLOB ENGINE_SOURCES CONFIGURE_DEPENDS src/engine/granular/*.cpp)
elseif(ENGINE STREQUAL passthrough)
    set(ENGINE_DEFINE SPK_ENGINE_PASSTHROUGH)
    set(ENGINE_SOURCES "")
elseif(ENGINE STREQUAL delay)
    set(ENGINE_DEFINE SPK_ENGINE_DELAY)
    set(ENGINE_SOURCES src/engine/delay/delay_engine.cpp)
elseif(ENGINE STREQUAL qdelay)
    set(ENGINE_DEFINE SPK_ENGINE_QDELAY)
    # QDelay flavor of the delay: its feedback diffuser is the header-only src/dsp/diffuser.h (no extra
    # source), so the engine .cpp is the only unit to compile. (GPLv3 -- see src/engine/qdelay/NOTICE.md.)
    set(ENGINE_SOURCES src/engine/qdelay/qdelay_engine.cpp)
elseif(ENGINE STREQUAL edrums)
    set(ENGINE_DEFINE SPK_ENGINE_EDRUMS)
    set(ENGINE_SOURCES src/engine/edrums/edrums_engine.cpp)
elseif(ENGINE STREQUAL reso)
    set(ENGINE_DEFINE SPK_ENGINE_RESO)
    # reso's DSP = Mutable Instruments Rings (vendored under src/engine/reso/thirdparty/, now rings-only)
    # + its stmlib support library, which was relocated to the shared common/ tree (dedup'd between reso
    # + mosc); mirror the Makefile's $(STMLIB_TP)/$(STMLIB_INC). reso source + rings .cc include "stmlib/..".
    set(RESO_TP ${CMAKE_CURRENT_SOURCE_DIR}/src/engine/reso/thirdparty)
    set(COMMON_TP ${CMAKE_CURRENT_SOURCE_DIR}/src/engine/common/thirdparty)
    set(STMLIB_TP ${COMMON_TP}/stmlib)
    set(ENGINE_SOURCES
        src/engine/reso/reso_engine.cpp
        ${RESO_TP}/rings/dsp/part.cc
        ${RESO_TP}/rings/dsp/string.cc
        ${RESO_TP}/rings/dsp/resonator.cc
        ${RESO_TP}/rings/dsp/fm_voice.cc
        ${RESO_TP}/rings/resources.cc
        ${STMLIB_TP}/dsp/units.cc
        ${STMLIB_TP}/utils/random.cc
        ${STMLIB_TP}/dsp/atan.cc
    )
    set(ENGINE_INCLUDES ${RESO_TP} ${COMMON_TP})
    # stmlib's filters use M_PI, which strict -std=c++17 does not expose from <cmath> on arm-none-eabi.
    set(ENGINE_DEFINES M_PI=3.14159265358979323846)
    # Rings (~30 KB of code+tables) overflows the 186 KB execution SRAM at -O2; build reso at -Os to fit.
    set(APP_OPT -Os)
elseif(ENGINE STREQUAL mosc)
    set(ENGINE_DEFINE SPK_ENGINE_MOSC)
    # QSPI-EXECUTE: the full 24-engine Plaits voice (~292 KB .text) overflows the 186 KB SRAM_EXEC, so mosc
    # runs from QSPI (alt_qspi.lds). The boot define is still BOOT_APP -- BOOT_SRAM/BOOT_QSPI differ only by
    # linker script. UNLIKE csound/chuck it still synthesises from the platform arena (no SPK_NO_ENGINE_ARENA)
    # and reuses the csound VTOR inject. PLAITS_USER_DATA_STUB: this board has no on-device flash patch store.
    # stmlib comes from the shared common/ tree (same dedup as reso). Build -Os to keep the SRAM tables small.
    set(MOSC_TP ${CMAKE_CURRENT_SOURCE_DIR}/src/engine/mosc/thirdparty)
    set(COMMON_TP ${CMAKE_CURRENT_SOURCE_DIR}/src/engine/common/thirdparty)
    set(STMLIB_TP ${COMMON_TP}/stmlib)
    set(ENGINE_DEFINES M_PI=3.14159265358979323846 PLAITS_USER_DATA_STUB)
    set(ENGINE_INCLUDES ${MOSC_TP} ${COMMON_TP})
    set(APP_OPT -Os)
    set(ENGINE_LDSCRIPT alt_qspi.lds)
    file(GLOB MOSC_PLAITS_CC CONFIGURE_DEPENDS
        ${MOSC_TP}/plaits/dsp/engine/*.cc
        ${MOSC_TP}/plaits/dsp/engine2/*.cc
        ${MOSC_TP}/plaits/dsp/physical_modelling/*.cc
        ${MOSC_TP}/plaits/dsp/speech/*.cc
    )
    set(ENGINE_SOURCES
        src/engine/mosc/mosc_engine.cpp
        src/engine/csound/spotykach_qspi_vtor.cpp
        ${MOSC_TP}/plaits/dsp/voice.cc
        ${MOSC_PLAITS_CC}
        ${MOSC_TP}/plaits/dsp/chords/chord_bank.cc
        ${MOSC_TP}/plaits/dsp/fm/algorithms.cc
        ${MOSC_TP}/plaits/dsp/fm/dx_units.cc
        ${MOSC_TP}/plaits/resources.cc
        ${STMLIB_TP}/dsp/units.cc
        ${STMLIB_TP}/utils/random.cc
        ${STMLIB_TP}/dsp/atan.cc
    )
elseif(ENGINE STREQUAL graincloud)
    # graincloud = a self-contained variant of the granular engine (copy under src/engine/graincloud/)
    # with its grain DSP replaced by a GrainflowLib cloud (gf_cloud.*).
    set(ENGINE_DEFINE SPK_ENGINE_GRAINCLOUD)
    set(GRAINCLOUD_TP ${CMAKE_CURRENT_SOURCE_DIR}/src/engine/graincloud/thirdparty)
    file(GLOB ENGINE_SOURCES CONFIGURE_DEPENDS src/engine/graincloud/*.cpp)
    set(ENGINE_INCLUDES ${GRAINCLOUD_TP})
    # M_PI for gfSyn under strict -std=c++17 on arm-none-eabi.
    set(ENGINE_DEFINES M_PI=3.14159265358979323846)
    # Granular + GrainflowLib templates overflow the 186 KB execution SRAM at -O2; build at -Os.
    set(APP_OPT -Os)
elseif(ENGINE STREQUAL tape)
    set(ENGINE_DEFINE SPK_ENGINE_TAPE)
    # Engine wrapper (tape_engine.cpp); its SD streaming service lives in src/hw/ (stream_deck.cpp,
    # fat_file.cpp, buffer.sdram.cpp -- picked up by the HW_SOURCES glob) with bodies guarded by
    # SPK_USE_STREAM, the feature gate every streaming engine opts into. Without that define those TUs
    # compile to empty objects and the link fails with undefined StreamDeck/FatFile symbols (the Makefile
    # sets it for tape too; this define was the parity gap that left the CMake tape build unlinkable).
    set(ENGINE_DEFINES SPK_USE_STREAM)
    set(ENGINE_SOURCES src/engine/tape/tape_engine.cpp)
elseif(ENGINE STREQUAL radio)
    set(ENGINE_DEFINE SPK_ENGINE_RADIO)
    # Dual virtual RadioMusic. Streams raw 16-bit-mono ".raw" stations from SD via the shared streaming
    # service in src/hw/ (stream_deck.cpp, fat_file.cpp, buffer.sdram.cpp -- HW_SOURCES glob), bodies
    # guarded by SPK_USE_STREAM (same gate as tape/shuttle); without it the link fails on undefined
    # StreamDeck/FatFile symbols.
    set(ENGINE_DEFINES SPK_USE_STREAM)
    set(ENGINE_SOURCES src/engine/radio/radio_engine.cpp)
elseif(ENGINE STREQUAL bard)
    set(ENGINE_DEFINE SPK_ENGINE_BARD)
    # The storyteller: bookmark-navigated audiobook decks. Streams books from SD via the shared streaming
    # service in src/hw/ (stream_deck.cpp, fat_file.cpp -- HW_SOURCES glob) and also WRITES its small resume
    # table through it, so SPK_USE_STREAM is required (same gate as tape/radio/pstretch).
    set(ENGINE_DEFINES SPK_USE_STREAM)
    set(ENGINE_SOURCES src/engine/bard/bard_engine.cpp)
    # -Os for the same reason the Makefile gives: WSOLA + the room reach ~94% SRAM_EXEC at -O2, leaving too
    # little headroom to work in. ~88% at -Os, and the M7 has ample compute margin for this engine.
    set(APP_OPT -Os)
elseif(ENGINE STREQUAL glitch)
    set(ENGINE_DEFINE SPK_ENGINE_GLITCH)
    # Dual lo-fi/circuit-bent glitch voice: 12 Noisferatu algorithms behind a header-only glitch::Voice.
    # Self-contained -- no SD/stream, no arena, no transport; just the engine wrapper. (GPLv3 -- see
    # src/engine/glitch/NOTICE.md.)
    set(ENGINE_SOURCES src/engine/glitch/glitch_engine.cpp)
elseif(ENGINE STREQUAL pstretch)
    set(ENGINE_DEFINE SPK_ENGINE_PSTRETCH)
    # Real-time clean-room PaulStretch ambient time-smear; self-contained vendored radix-2 FFT
    # (engine/pstretch/fft.h), no CMSIS-DSP. Rings + FFT scratch live in the SDRAM arena. The Phase-2
    # SD-file source streams clips via the shared streaming service, gated by SPK_USE_STREAM (as tape/radio).
    set(ENGINE_DEFINES SPK_USE_STREAM)
    set(ENGINE_SOURCES src/engine/pstretch/pstretch_engine.cpp)
elseif(ENGINE STREQUAL reverb)
    set(ENGINE_DEFINE SPK_ENGINE_REVERB)
    # Stereo reverb (Dattorro plate / Zita hall). Its DSP is the cyfaust-generated kernels
    # (faust_kernel_<name>.h, regenerated by `make faust-gen`) plus the hand-written arch shim -- both
    # header-only, so the engine wrapper is the only TU to compile.
    set(ENGINE_SOURCES src/engine/reverb/reverb_engine.cpp)
elseif(ENGINE STREQUAL shuttle)
    set(ENGINE_DEFINE SPK_ENGINE_SHUTTLE)
    # Buffer-based bipolar/reverse "varispeed shuttle" tape: 4 in-SDRAM mono buffers. LOAD (slot from SD
    # into RAM) opts into the shared streaming service via SPK_USE_STREAM -- the same gate the tape engine
    # sets, so stream_deck/fat_file/buffer.sdram compile their bodies and the platform pumps the StreamDeck.
    set(ENGINE_DEFINES SPK_USE_STREAM)
    set(ENGINE_SOURCES src/engine/shuttle/shuttle_engine.cpp)
elseif(ENGINE STREQUAL softcut)
    set(ENGINE_DEFINE SPK_ENGINE_SOFTCUT)
    # Dual-deck overdub looper on monome's vendored softcut-lib (5 .cpp under vendor/src). It loads loops
    # from SD, so it opts into the shared streaming service via SPK_USE_STREAM (same gate as tape/radio).
    # M_PI/M_PI_2: softcut's ReadWriteHead needs them and strict -std=c++17 hides them from <cmath> on
    # arm-none-eabi (the same idiom reso uses).
    set(SOFTCUT_TP ${CMAKE_CURRENT_SOURCE_DIR}/src/engine/softcut/vendor)
    set(ENGINE_DEFINES SPK_USE_STREAM M_PI=3.14159265358979323846 M_PI_2=1.57079632679489661923)
    set(ENGINE_INCLUDES ${SOFTCUT_TP}/include)
    file(GLOB SOFTCUT_VENDOR CONFIGURE_DEPENDS ${SOFTCUT_TP}/src/*.cpp)
    set(ENGINE_SOURCES src/engine/softcut/softcut_engine.cpp ${SOFTCUT_VENDOR})
elseif(ENGINE STREQUAL gigaverb)
    # gen~ engine: a Cycling '74 gen~ export bridged through gen-dsp's genlib-isolation layer plus the
    # shared src/engine/gen/ runtime (GenEngine + the arena-bound genlib). Mirrors the marker-managed
    # gigaverb block that scripts/gen_engine.py maintains in the Makefile. gen_exported.cpp is NOT a
    # source: it is #included via GEN_EXPORTED_CPP inside the bridge, so listing it would double-define.
    set(ENGINE_DEFINE SPK_ENGINE_GIGAVERB)
    set(GEN_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src/engine/gigaverb)
    set(ENGINE_DEFINES
        GENLIB_NO_JSON
        DAISY_EXT_NAME=gigaverb
        GEN_EXPORTED_NAME=gen_exported
        GEN_EXPORTED_HEADER="gen_exported.h"
        GEN_EXPORTED_CPP="gen_exported.cpp"
    )
    # gen~ codegen trips a raft of unused-function/variable/parameter warnings; the Make build silences
    # them for this engine, so match that to keep the build warning-clean.
    set(ENGINE_OPTIONS -Wno-unused-function -Wno-unused-variable -Wno-unused-parameter)
    set(ENGINE_INCLUDES ${GEN_DIR} ${GEN_DIR}/gen ${GEN_DIR}/gen/gen_dsp)
    set(ENGINE_SOURCES ${GEN_DIR}/_ext_daisy.cpp src/engine/gen/genlib_arena.cpp)
# >>> faust:chorus >>>
elseif(ENGINE STREQUAL chorus)
    set(ENGINE_DEFINE SPK_ENGINE_CHORUS)
    set(ENGINE_SOURCES)
# <<< faust:chorus <<<
# >>> faust:filter >>>
elseif(ENGINE STREQUAL filter)
    set(ENGINE_DEFINE SPK_ENGINE_FILTER)
    set(ENGINE_SOURCES)
# <<< faust:filter <<<
# >>> faust:voice >>>
elseif(ENGINE STREQUAL voice)
    set(ENGINE_DEFINE SPK_ENGINE_VOICE)
    set(ENGINE_SOURCES)
# <<< faust:voice <<<
elseif(ENGINE STREQUAL csound)
    set(ENGINE_DEFINE SPK_ENGINE_CSOUND)
    # QSPI-ONLY: links libcsound.a (~3.7 MB) -> runs from QSPI (alt_qspi.lds). Synthesises from its own
    # 12 MB SDRAM pool (csound_alloc.cpp), so SPK_NO_ENGINE_ARENA shrinks the platform arena; SPK_USE_STREAM
    # for the /csound/*.csd patch bank. Prereq: scripts/fetch_csound.sh has produced libcsound.a.
    set(CSOUND_BASE ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/csound/Daisy)
    set(ENGINE_DEFINES SPK_USE_STREAM SPK_NO_ENGINE_ARENA)
    set(ENGINE_INCLUDES ${CSOUND_BASE}/include/csound)
    set(ENGINE_LDSCRIPT alt_qspi.lds)
    set(ENGINE_SOURCES
        src/engine/csound/csound_engine.cpp
        src/engine/csound/csound_alloc.cpp
        src/engine/csound/spotykach_qspi_vtor.cpp
    )
    set(ENGINE_LIBS ${CSOUND_BASE}/lib/libcsound.a)
    # -u _printf_float: nano.specs omits float printf, which Csound needs. --wrap: route the C malloc family
    # to the SDRAM bump pool (csound_alloc.cpp); the platform heap stays in SRAM.
    set(ENGINE_LINK_OPTIONS -u _printf_float -Wl,--wrap=malloc,--wrap=free,--wrap=calloc,--wrap=realloc)
elseif(ENGINE STREQUAL chuck)
    set(ENGINE_DEFINE SPK_ENGINE_CHUCK)
    # QSPI-ONLY: links libchuck.a (~6 MB) -> runs from QSPI (alt_qspi_chuck.lds). Own 12 MB SDRAM pool
    # (chuck_alloc.cpp) -> SPK_NO_ENGINE_ARENA; SPK_USE_STREAM for the /chuck/*.ck patch bank. The ChucK
    # feature defines MUST match the set fetch_chuck.sh built libchuck.a with (ChucK class layouts / ABI).
    # Prereq: scripts/fetch_chuck.sh has produced libchuck.a + the shim sysroot.
    set(CHUCK_BASE ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/chuck)
    set(ENGINE_DEFINES
        SPK_USE_STREAM SPK_NO_ENGINE_ARENA
        __PLATFORM_LINUX__ __USE_CHUCK_YACC__
        CPU_IS_LITTLE_ENDIAN=1 CPU_IS_BIG_ENDIAN=0 TYPEOF_SF_COUNT_T=__INT64_TYPE__ SIZEOF_SF_COUNT_T=8
        __DISABLE_WATCHDOG__ __DISABLE_NETWORK__ __DISABLE_OTF_SERVER__
        __ALTER_HID__ __DISABLE_HID__ __DISABLE_SERIAL__ __DISABLE_ASYNCH_IO__ __DISABLE_THREADS__
        __DISABLE_KBHIT__ __DISABLE_PROMPTER__ __DISABLE_SHELL__ __OLDSCHOOL_RANDOM__
    )
    set(ENGINE_INCLUDES ${CHUCK_BASE}/src/core ${CHUCK_BASE}/Daisy/shim)
    set(ENGINE_LDSCRIPT alt_qspi_chuck.lds)
    set(ENGINE_SOURCES
        src/engine/chuck/chuck_engine.cpp
        src/engine/chuck/chuck_alloc.cpp
        src/engine/csound/spotykach_qspi_vtor.cpp
    )
    set(ENGINE_LIBS ${CHUCK_BASE}/Daisy/lib/libchuck.a)
    set(ENGINE_LINK_OPTIONS -u _printf_float -Wl,--wrap=malloc,--wrap=free,--wrap=calloc,--wrap=realloc)
    # ChucK's headers use C++ exceptions + RTTI and libchuck.a was built with both ON, so chuck_engine.cpp
    # must agree (ABI). Override the firmware's -fno-exceptions/-fno-rtti for JUST this TU (the rest of the
    # firmware keeps them off, so libstdc++'s exception machinery does not bloat the SRAM-tight platform).
    set_source_files_properties(src/engine/chuck/chuck_engine.cpp PROPERTIES COMPILE_OPTIONS "-fexceptions;-frtti")
else()
    message(FATAL_ERROR "Unknown ENGINE '${ENGINE}' - use granular, passthrough, delay, qdelay, edrums, reso, mosc, graincloud, tape, radio, bard, glitch, pstretch, reverb, shuttle, softcut, gigaverb, chorus, filter, voice, csound, or chuck")
endif()

# Source list = the Makefile's CPP_SOURCES, with the wildcard dirs expanded the same way.
file(GLOB TRANSPORT_SOURCES CONFIGURE_DEPENDS src/transport/*.cpp)
file(GLOB DSP_SOURCES       CONFIGURE_DEPENDS src/dsp/*.cpp)
file(GLOB HW_SOURCES        CONFIGURE_DEPENDS src/hw/*.cpp)
file(GLOB UI_SOURCES        CONFIGURE_DEPENDS src/ui/*.cpp)
file(GLOB MEMORY_SOURCES    CONFIGURE_DEPENDS src/memory/*.cpp)

set(FIRMWARE_NAME spotykach)
set(FIRMWARE_SOURCES
    main.cpp
    src/app.cpp
    src/version.cpp          # PARITY: defines firmware_banner()/version, referenced by app.cpp at boot
    ${ENGINE_SOURCES}
    src/engine/color.cpp
    src/engine/led.ring.cpp
    ${TRANSPORT_SOURCES}
    ${DSP_SOURCES}
    ${HW_SOURCES}
    ${UI_SOURCES}
    ${MEMORY_SOURCES}
)
set(DAISYSP_LIB DaisySP)

# PARITY (boot path): use the project's custom linker script, not the stock STM32H750IB_*.lds. SRAM
# engines use alt_sram.lds; the QSPI-execute engines (mosc/csound/chuck) set ENGINE_LDSCRIPT to
# alt_qspi.lds / alt_qspi_chuck.lds above. DaisyProject reads CUSTOM_LINKER_SCRIPT at include() time.
set(CUSTOM_LINKER_SCRIPT ${CMAKE_CURRENT_SOURCE_DIR}/${ENGINE_LDSCRIPT})

include(DaisyProject)

# PARITY (boot path, critical): the Make build defines BOOT_APP for the startup file, which
# makes Reset_Handler SKIP SystemInit() (the SRAM bootloader already ran it). DaisyProject does
# NOT set BOOT_APP when CUSTOM_LINKER_SCRIPT is used, so without this line the bootloaded app
# would re-run SystemInit() and likely fail to boot. BOOT_APP appears ONLY in startup_stm32h750xx.c,
# so defining it lib-wide changes nothing else.
target_compile_definitions(daisy PRIVATE BOOT_APP)

# The full investigation of the CMake-vs-Makefile SRAM_EXEC size gap (these two fixes + the residual
# newlib impure_data difference and the firmware-exit-stub option) is written up in docs/dev/cmake-gap.md.
# FORK GAP (size parity): the fork added -DDSY_DISABLE_USB_HOST to libDaisy's *Makefile* (so the Make
# libdaisy.a is built with it) but never to its CMakeLists.txt. Without it, fatfs.cpp (sys/fatfs.cpp)
# keeps its `#ifndef DSY_DISABLE_USB_HOST` USBH_Driver reference, which drags the entire unused USB-host
# MSC stack -> usb_host.cpp -> the Logger -> the USB-device CDC middleware into the link (~10 KB of SRAM
# .text/.data that --gc-sections cannot drop, because a global USB-host-handle static initializer roots
# it). The platform never uses USB-MSC host, so define it on the daisy lib to match the Make build's size.
target_compile_definitions(daisy PRIVATE DSY_DISABLE_USB_HOST)

# SIZE PARITY: the Make build compiles FatFs (ff.c/diskio.c/...) as ordinary app sources at the APP opt
# level (-O2, or -Os for reso/mosc/graincloud), whereas CMake builds it as a sub-library that inherits the
# directory -O3 -- so its f_*/dir_* functions come out ~2.4 KB larger. Force the FatFs target to the same
# APP_OPT so the FatFs .text matches the canonical build. (FatFs is the only libDaisy component the Make
# build compiles at app opt; everything else in libdaisy.a is -O3 in both.)
target_compile_options(FatFs PRIVATE ${APP_OPT})

# FORK GAP: the bleeptools fork added hid/midi_util.cpp (daisy::MidiTxMessage) to libDaisy's *Make*
# module list (core Makefile) but never to its CMakeLists.txt add_library(daisy ...). The Make build
# compiles it; the CMake build does not, so the app fails to link with undefined MidiTxMessage symbols.
# Patch it onto the target here rather than editing the vendored submodule. (per/pwm.cpp is also
# Make-only but the app uses the HAL TIM API directly, not daisy::Pwm, so it is unused and omitted.)
target_sources(daisy PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/lib/libDaisy/src/hid/midi_util.cpp)

# PARITY: app-level defines and the C++17 standard (DaisyProject sets the firmware target to C++14).
# USE_HAL_DRIVER / USE_FULL_LL_DRIVER are global C_DEFS in the Make build but only PRIVATE defines
# on the daisy CMake lib, so they do NOT reach app TUs here. They must: without USE_HAL_DRIVER the
# force-included stm32h7xx.h skips the HAL chain that drops <stddef.h> (::size_t) into global scope,
# and headers like detector.h that use bare `size_t` fail to compile.
# PARITY: firmware identity baked into the binary (src/version.cpp -> firmware_banner(), read by app.cpp
# at boot). The Makefile sets SPK_VERSION_STR from `git describe` and SPK_ENGINE_STR from ENGINE; mirror it.
execute_process(
    COMMAND git -C ${CMAKE_CURRENT_SOURCE_DIR} describe --tags --always
    OUTPUT_VARIABLE SPK_VERSION
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if(NOT SPK_VERSION)
    set(SPK_VERSION dev)
endif()

target_compile_definitions(${FIRMWARE_NAME} PRIVATE
    ${ENGINE_DEFINE}
    ${ENGINE_DEFINES}
    SPK_VERSION_STR="${SPK_VERSION}"
    SPK_ENGINE_STR="${ENGINE}"
    INFS_LOG_TARGET=daisy::LOGGER_EXTERNAL
    USE_HAL_DRIVER
    USE_FULL_LL_DRIVER
)

# PARITY: the old Makefile's config toggles (DEBUG=1 -> -DINFS_LOG=1, LOFI_INT16=1 -> -DLOFI_INT16=1).
# Passed straight through by the thin Make frontend (Makefile.cmake) as -DDEBUG=1 / -DLOFI_INT16=1.
if(DEBUG)
    target_compile_definitions(${FIRMWARE_NAME} PRIVATE INFS_LOG=1)
endif()
if(LOFI_INT16)
    target_compile_definitions(${FIRMWARE_NAME} PRIVATE LOFI_INT16=1)
endif()
# PARITY: Make uses strict -std=c++17 for the app (not gnu++17), so disable GNU extensions. The app
# does use the `asm` keyword, which strict c++17 rejects without -fasm -- Make supplies -fasm, added below.
set_target_properties(${FIRMWARE_NAME} PROPERTIES CXX_STANDARD 17 CXX_EXTENSIONS OFF)

# PARITY (app codegen flags): replicate the Make app TU flag set exactly. The trailing ${APP_OPT}
# (-O2 default, -Os for reso -- whose Rings DSP overflows SRAM at -O2) overrides the inherited directory
# -O3 for app TUs only (last -O on the line wins). DaisyProject already supplies
# -fno-exceptions/-fno-rtti/-fdata-sections/-ffunction-sections.
#   NOTE on -fshort-enums: in the Make build this is an APP-ONLY flag (libDaisy and DaisySP do NOT use
#   it), so app and library disagree on enum width. That asymmetry ships today and is hardware-proven,
#   so reproducing it here is the parity-correct choice -- it is no riskier than the current build.
target_compile_options(${FIRMWARE_NAME} PRIVATE
    -fasm
    -finline
    -finline-functions-called-once
    -fno-move-loop-invariants
    -fno-unwind-tables
    -fshort-enums
    -Wno-register
    ${APP_OPT}
    ${ENGINE_OPTIONS}
)

# PARITY: the Make build forces `-include stm32h7xx.h` globally and adds -Isrc/ -Ilib/.
target_include_directories(${FIRMWARE_NAME} PRIVATE src lib ${ENGINE_INCLUDES})
target_compile_options(${FIRMWARE_NAME} PRIVATE -include stm32h7xx.h)

# PARITY: the QSPI engines (csound/chuck) link an external static lib (libcsound.a / libchuck.a) and need
# extra link options (-u _printf_float + the SDRAM-pool malloc wraps). Both empty for every other engine.
if(ENGINE_LIBS)
    target_link_libraries(${FIRMWARE_NAME} PRIVATE ${ENGINE_LIBS})
endif()
if(ENGINE_LINK_OPTIONS)
    target_link_options(${FIRMWARE_NAME} PRIVATE ${ENGINE_LINK_OPTIONS})
endif()

# DFU plumbing (TODO flagged this as missing from the CMake path). Recreate the Makefile's
# program-dfu / program-boot as custom targets. BOOT_SRAM stores the app in QSPI at 0x90040000;
# the SRAM bootloader copies it to SRAM at boot. PID df11 = the Daisy bootloader's DFU id.
set(SPK_QSPI_ADDRESS 0x90040000)
set(SPK_INTERNAL_ADDRESS 0x08000000)
set(SPK_DAISY_PID df11)
add_custom_command(TARGET ${FIRMWARE_NAME} POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O binary -S $<TARGET_FILE:${FIRMWARE_NAME}> $<TARGET_FILE_DIR:${FIRMWARE_NAME}>/${FIRMWARE_NAME}.bin
    COMMENT "Generating ${FIRMWARE_NAME}.bin (DFU payload)")
add_custom_target(program-dfu
    COMMAND dfu-util -a 0 -s ${SPK_QSPI_ADDRESS}:leave -D $<TARGET_FILE_DIR:${FIRMWARE_NAME}>/${FIRMWARE_NAME}.bin -d ,0483:${SPK_DAISY_PID}
    DEPENDS ${FIRMWARE_NAME} USES_TERMINAL)
add_custom_target(program-boot
    COMMAND dfu-util -a 0 -s ${SPK_INTERNAL_ADDRESS}:leave -D ${CMAKE_CURRENT_SOURCE_DIR}/bootloader-spotykach-v2.bin -d ,0483:df11
    USES_TERMINAL)
