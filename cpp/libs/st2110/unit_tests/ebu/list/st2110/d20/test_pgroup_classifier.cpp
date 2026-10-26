#include "catch2/catch.hpp"
#include "ebu/list/st2110/d20/video_format_detector.h"
#include <cmath>

using namespace ebu_list;
using namespace ebu_list::st2110::d20;
using media::video::video_sampling;

//------------------------------------------------------------------------------

namespace
{
    // One line of SRDs of `pixels_per_srd` pixels, `bytes_per_srd` bytes each.
    pgroup_evidence line_of(int bytes_per_srd, int pixels_per_srd, int srds, bool odd_lines = true)
    {
        pgroup_evidence e;
        e.srd_bytes      = int64_t(bytes_per_srd) * srds;
        e.srd_pixels     = int64_t(pixels_per_srd) * srds;
        e.pixel_step_gcd = pixels_per_srd;
        e.odd_line_seen  = odd_lines;
        return e;
    }

    void append_bits(std::vector<uint8_t>& out, int& bit, int value, int depth)
    {
        for(int b = depth - 1; b >= 0; --b, ++bit)
        {
            if(bit % 8 == 0) out.push_back(0);
            out.back() |= static_cast<uint8_t>(((value >> b) & 1) << (7 - bit % 8));
        }
    }

    // A picture line with texture, packed as `sampling` at `depth` bits.
    // RGB has the same edges in R, G and B. YCbCr has its detail in Y, and
    // Cb and Cr change slowly.
    std::vector<uint8_t> textured_line(video_sampling sampling, int depth, int pixels)
    {
        std::vector<uint8_t> out;
        int bit         = 0;
        const auto full = (1 << depth) - 1;
        const auto code = [&](double v) { return static_cast<int>(std::lround(v * full)); };
        for(int x = 0; x < pixels; ++x)
        {
            const auto t  = 0.5 + 0.3 * std::sin(x / 7.0) + 0.1 * std::sin(x / 2.3);
            const auto cb = 0.45 + 0.02 * std::sin(x / 50.0);
            const auto cr = 0.53 + 0.02 * std::cos(x / 40.0);
            if(sampling == video_sampling::RGB_4_4_4)
            {
                append_bits(out, bit, code(0.9 * t + 0.05), depth);
                append_bits(out, bit, code(t), depth);
                append_bits(out, bit, code(0.8 * t + 0.1), depth);
            }
            else if(sampling == video_sampling::YCbCr_4_4_4)
            {
                append_bits(out, bit, code(cb), depth);
                append_bits(out, bit, code(t), depth);
                append_bits(out, bit, code(cr), depth);
            }
            else if(x % 2 == 0) // 4:2:2: Cb Y0 Cr Y1
            {
                const auto t1 = 0.5 + 0.3 * std::sin((x + 1) / 7.0) + 0.1 * std::sin((x + 1) / 2.3);
                append_bits(out, bit, code(cb), depth);
                append_bits(out, bit, code(t), depth);
                append_bits(out, bit, code(cr), depth);
                append_bits(out, bit, code(t1), depth);
            }
        }
        return out;
    }

    pgroup_evidence with_content(pgroup_evidence e, video_sampling sampling, int depth)
    {
        for(int i = 0; i < 32; ++i)
            e.payload_samples.push_back(textured_line(sampling, depth, 400));
        return e;
    }

    void require_format(const pgroup_evidence& e, video_sampling sampling, int depth)
    {
        const auto result = classify_pgroup(e);
        REQUIRE(result.has_value());
        REQUIRE(result->sampling == sampling);
        REQUIRE(result->color_depth == depth);
    }
} // namespace

SCENARIO("ST2110-20 pgroup classification")
{
    GIVEN("a pgroup size that only one format uses")
    {
        THEN("the size gives the format")
        {
            require_format(line_of(1200, 600, 4), video_sampling::YCbCr_4_2_2, 8);
            require_format(line_of(1200, 480, 4), video_sampling::YCbCr_4_2_2, 10);
            require_format(line_of(1200, 300, 4), video_sampling::YCbCr_4_2_2, 16);
            require_format(line_of(1200, 800, 4), video_sampling::YCbCr_4_2_0, 8);
            require_format(line_of(1200, 640, 4), video_sampling::YCbCr_4_2_0, 10);
        }
    }

    GIVEN("a 4:2:0 stream that numbers only the first line of each line pair")
    {
        THEN("the bits per offset are halved")
        {
            require_format(line_of(1200, 400, 4, false), video_sampling::YCbCr_4_2_0, 8);
        }
    }

    GIVEN("4:4:4 and RGB, which use the same pgroup")
    {
        THEN("the pixel data selects the sampling")
        {
            const auto e = line_of(1200, 320, 4);
            require_format(with_content(e, video_sampling::RGB_4_4_4, 10), video_sampling::RGB_4_4_4, 10);
            require_format(with_content(e, video_sampling::YCbCr_4_4_4, 10), video_sampling::YCbCr_4_4_4, 10);
            const auto e12 = line_of(1197, 266, 4);
            require_format(with_content(e12, video_sampling::RGB_4_4_4, 12), video_sampling::RGB_4_4_4, 12);
        }
    }

    GIVEN("24 bits per pixel: 4:2:2 12-bit or 4:4:4 8-bit")
    {
        THEN("the pixel data selects the format")
        {
            const auto e = line_of(1200, 400, 4);
            require_format(with_content(e, video_sampling::YCbCr_4_2_2, 12), video_sampling::YCbCr_4_2_2, 12);
            require_format(with_content(e, video_sampling::YCbCr_4_4_4, 8), video_sampling::YCbCr_4_4_4, 8);
            require_format(with_content(e, video_sampling::RGB_4_4_4, 8), video_sampling::RGB_4_4_4, 8);
        }

        THEN("an odd pixel step is possible only for 4:4:4")
        {
            require_format(with_content(line_of(1197, 399, 4), video_sampling::YCbCr_4_4_4, 8),
                           video_sampling::YCbCr_4_4_4, 8);
        }
    }

    GIVEN("no SRD pair, or a size of no format")
    {
        THEN("there is no result")
        {
            REQUIRE_FALSE(classify_pgroup(pgroup_evidence{}).has_value());
            REQUIRE_FALSE(classify_pgroup(line_of(1000, 300, 4)).has_value());
        }
    }
}
