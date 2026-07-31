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
#include <cmath>   // std::math (transitive-include hygiene; host build)

#include <algorithm>

namespace infrasonic
{

/// @brief Helper for pos/neg VOct calibration.
///        Based on libDaisy VoctCalibration ((C) Electrosmith, see MIT license)
///        but with addition of negative CV offset support and 0V neutral preservation.
class CalibratedVOct
{
  public:
    struct ReferenceReadings
    {
        float neg3v;
        float neg1v;
        float pos1v;
        float pos3v;

        ReferenceReadings() { Defaults(); };
        ~ReferenceReadings() = default;

        inline void Defaults()
        {
            neg3v = -0.6f;
            neg1v = -0.2f;
            pos1v = 0.2f;
            pos3v = 0.6f;
        }
    };

    CalibratedVOct()  = default;
    ~CalibratedVOct() = default;

    inline void Init(const ReferenceReadings &readings)
    {
        pos_scale_  = 24.0f / (readings.pos3v - readings.pos1v);
        pos_offset_ = 12.0f - pos_scale_ * readings.pos1v;
        pos_tz_     = (kZeroAdjustThresh - pos_offset_) / pos_scale_;
        neg_scale_  = -24.0f / (readings.neg3v - readings.neg1v);
        neg_offset_ = 12.0f + neg_scale_ * readings.neg1v;
        neg_tz_     = (-kZeroAdjustThresh + neg_offset_) / neg_scale_;
    }

    // Returns VOct offset in semitones (note numbers)
    inline float Process(float norm_cv)
    {
        // preserve 0V == neutral
        if(std::fabs(norm_cv) < 1e-6f)
        {
            return 0.0f;
        }

        // positive case
        if(norm_cv >= 0.0f)
        {
            // preserve 0V == neutral
            if(norm_cv < pos_tz_)
            {
                return (norm_cv * kZeroAdjustThresh) / pos_tz_;
            }
            return pos_offset_ + norm_cv * pos_scale_;
        }

        // negative case
        // preserve 0V == neutral
        if(norm_cv > neg_tz_)
        {
            return (-norm_cv * kZeroAdjustThresh) / neg_tz_;
        }
        return norm_cv * neg_scale_ - neg_offset_;
    }

  private:
    static constexpr float kZeroAdjustThresh = 0.05f; // semitones
    float                  pos_scale_, pos_offset_;
    float                  neg_scale_, neg_offset_;
    float                  pos_tz_, neg_tz_;
};

} // namespace infrasonic
