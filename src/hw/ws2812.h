// Copyright 2024 Infrasonic Audio LLC
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
//
// See http://creativecommons.org/licenses/MIT/ for more information.
//
// -----------------------------------------------------------------------------
#pragma once
#include <cstdint>   // uint*_t (transitive-include hygiene; host build)

#include <daisy.h>

namespace infrasonic
{

/**
 * @brief Driver class for Ws2812-style LEDs. Currently limited to 128 LEDs max
 *        and one instance used per daisy due to shared DMA buffer.
 *
 * @warning This only works with a modified version of libDaisy with a method to
 *          obtain the timer's HAL handle directly, it does not work with mainline libDaisy
 */
class Ws2812
{
  public:
    static constexpr uint8_t kMaxNumLEDs = 128;

    struct Config
    {
        // TODO: this really should be more abstracted
        // but libDaisy doesn't have a mature hardware timer
        // CC/OC implementation
        enum TimerChannel
        {
            CH1,
            CH2,
            CH3,
            CH4,
        };

        using TimerPeripheral = daisy::TimerHandle::Config::Peripheral;

        uint8_t num_leds;

        daisy::Pin tim_pin;

        // NOTE: this only works with TIM3 or TIM4 currently,
        // could support others with some modifications
        TimerPeripheral tim_periph;
        TimerChannel    tim_channel;

        // These defaults are fine for most WS2182-style LEDs
        uint32_t symbol_length_ns = 1250;
        uint32_t zero_high_ns     = 450;
        uint32_t one_high_ns      = 850;
    };

    Ws2812()  = default;
    ~Ws2812() = default;

    void Init(const Config& config);
    void SetGamma(float gamma);

    // Set a global brightness max, range 0.1 - 1.0
    // This sets max actual brightness transmitted to LEDs
    // with brightness value scaled to new max range.
    void SetBrightnessLimit(float limit);

    // For all these methods, always use FULL BRIGHTNESS 24-bit colors -
    // use brightness param to set LED brightness to take advantage of internal
    // gamma correction and temporal dithering
    void
    Set(uint8_t idx, uint8_t r, uint8_t g, uint8_t b, float brightness = 1.0f);
    void Set(uint8_t idx, uint32_t color, float brightness = 1.0f);
    void Fill(uint32_t color, float brightness = 1.0f);

    void Clear();
    void Show();

    class Impl;

  private:
    Impl*   pimpl_;
    uint8_t num_leds_;
};

} // namespace balsam
