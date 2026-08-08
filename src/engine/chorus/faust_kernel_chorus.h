// SYNTHUX ACADEMY /////////////////////////////////////////
// SPOTYKACH ///////////////////////////////////////////////
#pragma once

// GENERATED FILE - do not edit by hand. Regenerate with `make faust-kernels` (cyfaust cpp backend).
// Source: src/engine/chorus/chorus.dsp. The generated `class mydsp` is wrapped in namespace spotykach::fx_chorus; its
// dsp/UI/Meta base types resolve to the shared arch shim (see engine/faust_arch.h).

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <math.h>
#include "engine/faust_arch.h"

namespace spotykach { namespace fx_chorus {
/* ------------------------------------------------------------
name: "chorus"
Code generated with Faust 2.85.9 (https://faust.grame.fr)
Compilation options: -lang cpp -fpga-mem-th 4 -ct 1 -es 1 -mcd 16 -mdd 1024 -mdy 33 -single -ftz 0
------------------------------------------------------------ */

#ifndef  __fx_chorus_H__
#define  __fx_chorus_H__

#ifndef FAUSTFLOAT
#define FAUSTFLOAT float
#endif 


#ifndef FAUSTCLASS 
#define FAUSTCLASS mydsp
#endif

#ifdef __APPLE__ 
#define exp10f __exp10f
#define exp10 __exp10
#endif

#if defined(_WIN32)
#define RESTRICT __restrict
#else
#define RESTRICT __restrict__
#endif


class mydsp : public dsp {
	
 private:
	
	int IOTA0;
	float fVec0[4096];
	int fSampleRate;
	float fConst0;
	float fConst1;
	FAUSTFLOAT fHslider0;
	float fConst2;
	int iVec1[2];
	float fRec0[2];
	FAUSTFLOAT fHslider1;
	float fRec1[2];
	FAUSTFLOAT fHslider2;
	float fRec2[2];
	float fConst3;
	FAUSTFLOAT fHslider3;
	float fRec4[2];
	float fRec3[2];
	float fVec2[4096];
	float fConst4;
	float fRec5[2];
	
 public:
	mydsp() {
	}
	
	mydsp(const mydsp&) = default;
	
	virtual ~mydsp() = default;
	
	mydsp& operator=(const mydsp&) = default;
	
	void metadata(Meta* m) { 
		m->declare("compile_options", "-lang cpp -fpga-mem-th 4 -ct 1 -es 1 -mcd 16 -mdd 1024 -mdy 33 -single -ftz 0");
		m->declare("delays.lib/name", "Faust Delay Library");
		m->declare("delays.lib/version", "1.2.0");
		m->declare("description", "Stereo chorus - sk-engines Faust demo engine (generated wrapper).");
		m->declare("filename", "chorus");
		m->declare("maths.lib/author", "GRAME");
		m->declare("maths.lib/copyright", "GRAME");
		m->declare("maths.lib/license", "LGPL with exception");
		m->declare("maths.lib/name", "Faust Math Library");
		m->declare("maths.lib/version", "2.9.0");
		m->declare("name", "chorus");
		m->declare("oscillators.lib/lf_sawpos:author", "Bart Brouns, revised by Stéphane Letz");
		m->declare("oscillators.lib/lf_sawpos:licence", "STK-4.3");
		m->declare("oscillators.lib/name", "Faust Oscillator Library");
		m->declare("oscillators.lib/version", "1.7.0");
		m->declare("platform.lib/name", "Generic Platform Library");
		m->declare("platform.lib/version", "1.3.0");
		m->declare("signals.lib/name", "Faust Routing Library");
		m->declare("signals.lib/version", "1.6.0");
	}

	virtual int getNumInputs() {
		return 2;
	}
	virtual int getNumOutputs() {
		return 2;
	}
	
	static void classInit(int sample_rate) {
	}
	
	virtual void instanceConstants(int sample_rate) {
		fSampleRate = sample_rate;
		fConst0 = std::min<float>(1.92e+05f, std::max<float>(1.0f, static_cast<float>(fSampleRate)));
		fConst1 = 44.1f / fConst0;
		fConst2 = 1.0f - fConst1;
		fConst3 = 1.0f / fConst0;
		fConst4 = 1.03f / fConst0;
	}
	
	virtual void instanceResetUserInterface() {
		fHslider0 = static_cast<FAUSTFLOAT>(0.5f);
		fHslider1 = static_cast<FAUSTFLOAT>(0.4f);
		fHslider2 = static_cast<FAUSTFLOAT>(0.5f);
		fHslider3 = static_cast<FAUSTFLOAT>(0.3f);
	}
	
	virtual void instanceClear() {
		IOTA0 = 0;
		for (int l0 = 0; l0 < 4096; l0 = l0 + 1) {
			fVec0[l0] = 0.0f;
		}
		for (int l1 = 0; l1 < 2; l1 = l1 + 1) {
			iVec1[l1] = 0;
		}
		for (int l2 = 0; l2 < 2; l2 = l2 + 1) {
			fRec0[l2] = 0.0f;
		}
		for (int l3 = 0; l3 < 2; l3 = l3 + 1) {
			fRec1[l3] = 0.0f;
		}
		for (int l4 = 0; l4 < 2; l4 = l4 + 1) {
			fRec2[l4] = 0.0f;
		}
		for (int l5 = 0; l5 < 2; l5 = l5 + 1) {
			fRec4[l5] = 0.0f;
		}
		for (int l6 = 0; l6 < 2; l6 = l6 + 1) {
			fRec3[l6] = 0.0f;
		}
		for (int l7 = 0; l7 < 4096; l7 = l7 + 1) {
			fVec2[l7] = 0.0f;
		}
		for (int l8 = 0; l8 < 2; l8 = l8 + 1) {
			fRec5[l8] = 0.0f;
		}
	}
	
	virtual void init(int sample_rate) {
		classInit(sample_rate);
		instanceInit(sample_rate);
	}
	
	virtual void instanceInit(int sample_rate) {
		instanceConstants(sample_rate);
		instanceResetUserInterface();
		instanceClear();
	}
	
	virtual mydsp* clone() {
		return new mydsp(*this);
	}
	
	virtual int getSampleRate() {
		return fSampleRate;
	}
	
	virtual void buildUserInterface(UI* ui_interface) {
		ui_interface->openVerticalBox("chorus");
		ui_interface->addHorizontalSlider("delay", &fHslider1, FAUSTFLOAT(0.4f), FAUSTFLOAT(0.0f), FAUSTFLOAT(1.0f), FAUSTFLOAT(0.001f));
		ui_interface->addHorizontalSlider("depth", &fHslider2, FAUSTFLOAT(0.5f), FAUSTFLOAT(0.0f), FAUSTFLOAT(1.0f), FAUSTFLOAT(0.001f));
		ui_interface->addHorizontalSlider("mix", &fHslider0, FAUSTFLOAT(0.5f), FAUSTFLOAT(0.0f), FAUSTFLOAT(1.0f), FAUSTFLOAT(0.001f));
		ui_interface->addHorizontalSlider("rate", &fHslider3, FAUSTFLOAT(0.3f), FAUSTFLOAT(0.0f), FAUSTFLOAT(1.0f), FAUSTFLOAT(0.001f));
		ui_interface->closeBox();
	}
	
	virtual void compute(int count, FAUSTFLOAT** RESTRICT inputs, FAUSTFLOAT** RESTRICT outputs) {
		FAUSTFLOAT* input0 = inputs[0];
		FAUSTFLOAT* input1 = inputs[1];
		FAUSTFLOAT* output0 = outputs[0];
		FAUSTFLOAT* output1 = outputs[1];
		float fSlow0 = fConst1 * static_cast<float>(fHslider0);
		float fSlow1 = fConst1 * static_cast<float>(fHslider1);
		float fSlow2 = fConst1 * static_cast<float>(fHslider2);
		float fSlow3 = fConst1 * static_cast<float>(fHslider3);
		for (int i0 = 0; i0 < count; i0 = i0 + 1) {
			float fTemp0 = static_cast<float>(input0[i0]);
			fVec0[IOTA0 & 4095] = fTemp0;
			iVec1[0] = 1;
			fRec0[0] = fSlow0 + fConst2 * fRec0[1];
			float fTemp1 = 1.0f - fRec0[0];
			fRec1[0] = fSlow1 + fConst2 * fRec1[1];
			float fTemp2 = 0.015f * fRec1[0];
			fRec2[0] = fSlow2 + fConst2 * fRec2[1];
			int iTemp3 = 1 - iVec1[1];
			fRec4[0] = fSlow3 + fConst2 * fRec4[1];
			float fTemp4 = 5.0f * fRec4[0] + 0.05f;
			float fTemp5 = ((iTemp3) ? 0.0f : fRec3[1] + fConst3 * fTemp4);
			fRec3[0] = fTemp5 - std::floor(fTemp5);
			float fTemp6 = fConst0 * (fTemp2 + 0.0025f * fRec2[0] * (std::sin(6.2831855f * fRec3[0]) + 1.0f) + 0.005f);
			int iTemp7 = static_cast<int>(fTemp6);
			float fTemp8 = std::floor(fTemp6);
			output0[i0] = static_cast<FAUSTFLOAT>(fTemp0 * fTemp1 + fRec0[0] * (fVec0[(IOTA0 - std::min<int>(2049, std::max<int>(0, iTemp7))) & 4095] * (fTemp8 + (1.0f - fTemp6)) + (fTemp6 - fTemp8) * fVec0[(IOTA0 - std::min<int>(2049, std::max<int>(0, iTemp7 + 1))) & 4095]));
			float fTemp9 = static_cast<float>(input1[i0]);
			fVec2[IOTA0 & 4095] = fTemp9;
			float fTemp10 = ((iTemp3) ? 0.0f : fRec5[1] + fConst4 * fTemp4);
			fRec5[0] = fTemp10 - std::floor(fTemp10);
			float fTemp11 = fConst0 * (fTemp2 + 0.0025f * fRec2[0] * (std::sin(6.2831855f * fRec5[0]) + 1.0f) + 0.005f);
			int iTemp12 = static_cast<int>(fTemp11);
			float fTemp13 = std::floor(fTemp11);
			output1[i0] = static_cast<FAUSTFLOAT>(fTemp9 * fTemp1 + fRec0[0] * (fVec2[(IOTA0 - std::min<int>(2049, std::max<int>(0, iTemp12))) & 4095] * (fTemp13 + (1.0f - fTemp11)) + (fTemp11 - fTemp13) * fVec2[(IOTA0 - std::min<int>(2049, std::max<int>(0, iTemp12 + 1))) & 4095]));
			IOTA0 = IOTA0 + 1;
			iVec1[1] = iVec1[0];
			fRec0[1] = fRec0[0];
			fRec1[1] = fRec1[0];
			fRec2[1] = fRec2[0];
			fRec4[1] = fRec4[0];
			fRec3[1] = fRec3[0];
			fRec5[1] = fRec5[0];
		}
	}

};

#endif
} } // namespace spotykach::fx_chorus
