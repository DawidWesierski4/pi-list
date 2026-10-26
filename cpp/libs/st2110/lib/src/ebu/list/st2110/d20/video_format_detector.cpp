#include "ebu/list/st2110/d20/video_format_detector.h"
#include "ebu/list/st2110/d20/packet.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <numeric>

using namespace ebu_list::st2110::d20;
using namespace ebu_list::st2110;
using namespace ebu_list;

//------------------------------------------------------------------------------
namespace
{
    constexpr auto maximum_packets_per_frame = 30000; // TODO: this is enough for UHD
    constexpr auto minimum_packets_per_frame = 300;

    // Bounds of the payload sample that classify_pgroup() reads.
    constexpr auto payload_sample_every         = 7; // keep one SRD of each 7
    constexpr auto maximum_payload_samples      = 512;
    constexpr auto maximum_payload_sample_bytes = 1440;

    using media::video::video_sampling;

    struct candidate
    {
        video_sampling sampling;
        int depth;
    };

    // Reads sample number `index` of `depth` bits from a big-endian bit stream.
    int read_sample(const std::vector<uint8_t>& data, size_t index, int depth)
    {
        const auto first_bit = index * depth;
        int value            = 0;
        for(int b = 0; b < depth; ++b)
        {
            const auto bit = first_bit + b;
            value          = (value << 1) | ((data[bit / 8] >> (7 - bit % 8)) & 1);
        }
        return value;
    }

    // Components of a pgroup, in wire order. 0 = Cb or R, 1 = Y or G,
    // 2 = Cr or B.
    std::vector<int> component_order(video_sampling sampling)
    {
        if(sampling == video_sampling::YCbCr_4_2_2) return {0, 1, 2, 1};
        return {0, 1, 2};
    }

    // Splits the payload sample into one sequence of normalized values
    // (0..1) for each component. A pgroup is never split, so each payload
    // starts on component 0.
    std::array<std::vector<double>, 3> split_components(const pgroup_evidence& evidence, const candidate& c)
    {
        std::array<std::vector<double>, 3> components;
        const auto order     = component_order(c.sampling);
        const auto max_value = static_cast<double>((1 << c.depth) - 1);
        for(const auto& data : evidence.payload_samples)
        {
            const auto samples = data.size() * 8 / c.depth;
            const auto whole   = samples - samples % order.size();
            for(size_t i = 0; i < whole; ++i)
            {
                components[order[i % order.size()]].push_back(read_sample(data, i, c.depth) / max_value);
            }
        }
        return components;
    }

    // Mean step between two neighbour values of the same component. Pixel
    // data is smooth, so the right layout gives the smallest value.
    double roughness(const pgroup_evidence& evidence, const candidate& c)
    {
        const auto components = split_components(evidence, c);
        double sum            = 0;
        size_t count          = 0;
        for(const auto& values : components)
        {
            for(size_t i = 1; i < values.size(); ++i)
            {
                sum += std::abs(values[i] - values[i - 1]);
                ++count;
            }
        }
        return count == 0 ? 0 : sum / count;
    }

    // Pearson correlation of two sequences of the same size.
    double correlation(const std::vector<double>& a, const std::vector<double>& b)
    {
        const auto n = std::min(a.size(), b.size());
        if(n < 2) return 0;
        double ma = 0, mb = 0;
        for(size_t i = 0; i < n; ++i)
        {
            ma += a[i];
            mb += b[i];
        }
        ma /= n;
        mb /= n;
        double ab = 0, aa = 0, bb = 0;
        for(size_t i = 0; i < n; ++i)
        {
            ab += (a[i] - ma) * (b[i] - mb);
            aa += (a[i] - ma) * (a[i] - ma);
            bb += (b[i] - mb) * (b[i] - mb);
        }
        return (aa == 0 || bb == 0) ? 0 : ab / std::sqrt(aa * bb);
    }

    // YCbCr 4:4:4 and RGB use the same pgroup, so only the pixel data can
    // tell. In RGB an edge is in all three components: their horizontal
    // steps correlate and have the same size. In YCbCr the detail is in Y:
    // the Cb and Cr steps are smaller and do not correlate with the Y steps.
    // On the MTL test media (Penguin, ParkJoy, Crosswalk) the score is +0.28
    // or more for RGB and -0.39 or less for YCbCr. Returns true for YCbCr.
    bool looks_like_ycbcr(const pgroup_evidence& evidence, int depth)
    {
        std::array<std::vector<double>, 3> steps;
        for(const auto& data : evidence.payload_samples)
        {
            pgroup_evidence one;
            one.payload_samples.push_back(data);
            const auto c = split_components(one, {video_sampling::YCbCr_4_4_4, depth});
            for(size_t k = 0; k < 3; ++k)
                for(size_t i = 1; i < c[k].size(); ++i)
                    steps[k].push_back(c[k][i] - c[k][i - 1]);
        }

        const auto mean_abs = [](const std::vector<double>& v) {
            double sum = 0;
            for(const auto x : v)
                sum += std::abs(x);
            return v.empty() ? 0 : sum / v.size();
        };
        const auto middle_detail = mean_abs(steps[1]);
        if(middle_detail == 0) return true; // a flat picture: no evidence

        const auto detail_ratio     = (mean_abs(steps[0]) + mean_abs(steps[2])) / (2 * middle_detail);
        const auto step_correlation = (correlation(steps[0], steps[1]) + correlation(steps[2], steps[1])) / 2;
        return step_correlation + (detail_ratio - 1) < 0;
    }

    std::tuple<int, int> get_dimensions_from_max_line(int max_line, bool is_field_based)
    {
        if(is_field_based)
        {
            switch(max_line)
            {
            case 239: return {720, 480};
            case 242: return {720, 486};
            case 287: return {720, 576};
            case 539: return {1920, 1080};

            default: break;
            }
        }
        else
        {
            switch(max_line)
            {
            case 719: return {1280, 720};
            case 1079: return {1920, 1080};
            case 2159: return {3840, 2160}; // TODO: calculate width correctly

            default: break;
            }
        }

        // TODO: check this
        return {0, 0};
    }
} // namespace

//------------------------------------------------------------------------------

detector::status_description line_data_analyzer::handle_data(const rtp::packet& packet)
{
    auto& sdu = packet.sdu;

    constexpr auto minimum_size = ssizeof<raw_extended_sequence_number>() + ssizeof<raw_line_header>();
    if(sdu.view().size() < minimum_size)
    {
        // logger()->error("Packet size smaller than minimum: {}", sdu.view().size());
        return detector::status_description{/*.state*/ detector::state::invalid,
                                            /*.error_code*/ "STATUS_CODE_FORMAT_NO_MINIMUM_SIZE"};
    }

    auto p         = sdu.view().data();
    const auto end = sdu.view().data() + sdu.view().size();

    // skip esn
    p += sizeof(raw_extended_sequence_number);

    std::vector<int> srd_lengths;
    bool keep_payload = false;

    while(p + ssizeof<raw_line_header>() <= end)
    {
        const auto line_header = line_header_lens(*reinterpret_cast<const raw_line_header*>(p));
        p += sizeof(raw_line_header);

        const auto line   = int(line_header.line_number());
        const auto field  = int(line_header.field_identification());
        const auto offset = int(line_header.offset());
        const auto length = int(line_header.length());
        if(line == last_line_ && field == last_field_ && offset > last_offset_ && last_length_ > 0)
        {
            const auto step = offset - last_offset_;
            evidence_.srd_bytes += last_length_;
            evidence_.srd_pixels += step;
            evidence_.pixel_step_gcd = std::gcd(evidence_.pixel_step_gcd, step);
        }
        evidence_.odd_line_seen |= (line % 2 != 0);
        last_line_   = line;
        last_field_  = field;
        last_offset_ = offset;
        last_length_ = length;
        srd_lengths.push_back(length);
        keep_payload |= (srd_count_++ % payload_sample_every == 0);

        if(!packet.info.rtp().marker() && int(line_header.line_number()) >= line_number_)
        {
            line_number_ = int(line_header.line_number());
        }
        else if(packet.info.rtp().marker())
        {
            line_number_ = 0;
        }
        else
        {
            logger()->error("ERROR - Packet not marked. Packet Line Number {}. Previous Packet Line Number {}",
                            int(line_header.line_number()), line_number_);
            return detector::status_description{/*.state*/ detector::state::invalid,
                                                /*.error_code*/ "STATUS_CODE_VIDEO_INVALID_LINE_NUMBER"};
        }

        max_line_number_ = std::max(max_line_number_, int(line_header.line_number()));

        is_field_based_ |= (line_header.field_identification() != 0);

        if(!line_header.continuation()) break;
    }

    if(keep_payload && evidence_.payload_samples.size() < maximum_payload_samples)
    {
        // The payload of each SRD follows the last SRD header, in SRD order.
        for(const auto length : srd_lengths)
        {
            if(p + length > end) break;
            const auto size = std::min(length, maximum_payload_sample_bytes);
            evidence_.payload_samples.emplace_back(reinterpret_cast<const uint8_t*>(p),
                                                   reinterpret_cast<const uint8_t*>(p) + size);
            p += length;
        }
    }

    return detector::status_description{/*.state*/ detector::state::detecting,
                                        /*.error_code*/ "STATUS_CODE_FORMAT_DETECTING"};
}

int line_data_analyzer::max_line_number() const noexcept
{
    return max_line_number_;
}

bool line_data_analyzer::is_field_based() const noexcept
{
    return is_field_based_;
}

const pgroup_evidence& line_data_analyzer::evidence() const noexcept
{
    return evidence_;
}

//------------------------------------------------------------------------------

std::optional<sampling_and_depth> d20::classify_pgroup(const pgroup_evidence& evidence)
{
    if(evidence.srd_pixels == 0) return std::nullopt;

    // Bits per pixel offset. RFC 4175 4:2:0 puts two lines in one SRD, so a
    // stream with no odd line number has two times the bits per offset.
    const auto bits_per_offset = 8.0 * evidence.srd_bytes / evidence.srd_pixels;
    const auto near            = [&](double bits) { return std::abs(bits_per_offset - bits) < 0.05; };

    const bool line_pairs = !evidence.odd_line_seen;
    if(line_pairs)
    {
        // 12, 15, 18 and 24 bits per pixel, read on a line pair.
        if(near(24) && evidence.pixel_step_gcd % 2 == 0) return sampling_and_depth{video_sampling::YCbCr_4_2_0, 8};
        if(near(30) && evidence.pixel_step_gcd % 4 == 0) return sampling_and_depth{video_sampling::YCbCr_4_2_0, 10};
        if(near(36) && evidence.pixel_step_gcd % 2 == 0) return sampling_and_depth{video_sampling::YCbCr_4_2_0, 12};
    }

    // 4:2:0 numbered on each line (MTL), 4:2:2, and 4:4:4 / RGB.
    if(near(12)) return sampling_and_depth{video_sampling::YCbCr_4_2_0, 8};
    if(near(15)) return sampling_and_depth{video_sampling::YCbCr_4_2_0, 10};
    if(near(18)) return sampling_and_depth{video_sampling::YCbCr_4_2_0, 12};
    if(near(16)) return sampling_and_depth{video_sampling::YCbCr_4_2_2, 8};
    if(near(20)) return sampling_and_depth{video_sampling::YCbCr_4_2_2, 10};
    if(near(32)) return sampling_and_depth{video_sampling::YCbCr_4_2_2, 16};

    auto depth = 0;
    if(near(24))
    {
        // 4:2:2 12-bit (6 bytes for 2 pixels) or 4:4:4 8-bit (3 bytes for 1
        // pixel). An odd pixel step is possible only for 4:4:4. Otherwise the
        // pixel data must tell.
        const auto yuv422_12 = candidate{video_sampling::YCbCr_4_2_2, 12};
        const auto full_8    = candidate{video_sampling::YCbCr_4_4_4, 8};
        if(evidence.pixel_step_gcd % 2 == 0 && roughness(evidence, yuv422_12) < roughness(evidence, full_8))
        {
            return sampling_and_depth{video_sampling::YCbCr_4_2_2, 12};
        }
        depth = 8;
    }
    else if(near(30))
        depth = 10;
    else if(near(36))
        depth = 12;
    else if(near(48))
        depth = 16;
    else
        return std::nullopt;

    // YCbCr 4:4:4 and RGB use the same pgroup. Only the pixel data tells.
    return sampling_and_depth{
        looks_like_ycbcr(evidence, depth) ? video_sampling::YCbCr_4_4_4 : video_sampling::RGB_4_4_4, depth};
}

//------------------------------------------------------------------------------

video_format_detector::video_format_detector() : detector_({maximum_packets_per_frame, minimum_packets_per_frame})
{
}

detector::status_description video_format_detector::handle_data(const rtp::packet& packet)
{
    const auto la_result = line_analyzer_.handle_data(packet);
    if(la_result.state == detector::state::invalid) return la_result;
    const auto sa_result = spacing_analyzer_.handle_data(packet);
    if(sa_result.state == detector::state::invalid) return sa_result;

    return detector_.handle_data(packet);
}

detector::details video_format_detector::get_details() const
{
    auto result              = video_description{};
    result.packets_per_frame = detector_.packets_per_frame();
    result.rate              = detector_.rate();

    result.scan_type =
        (line_analyzer_.is_field_based() ? media::video::scan_type::INTERLACED : media::video::scan_type::PROGRESSIVE);

    const auto [width, height] =
        get_dimensions_from_max_line(line_analyzer_.max_line_number(), line_analyzer_.is_field_based());
    result.dimensions.width  = static_cast<uint16_t>(width);
    result.dimensions.height = static_cast<uint16_t>(height);
    result.schedule          = spacing_analyzer_.get_schedule();

    // An SDP or the stream "help" API can still overwrite these values.
    if(const auto format = classify_pgroup(line_analyzer_.evidence()))
    {
        result.sampling    = format->sampling;
        result.color_depth = static_cast<uint16_t>(format->color_depth);
    }
    else
    {
        logger()->warn("ST 2110-20: pgroup not known ({} bytes for {} pixels). Using YCbCr-4:2:2 10-bit.",
                       line_analyzer_.evidence().srd_bytes, line_analyzer_.evidence().srd_pixels);
        result.color_depth = 10;
        result.sampling    = media::video::video_sampling::YCbCr_4_2_2;
    }
    result.colorimetry = media::video::colorimetry::BT709;

    return result;
}

detector::full_type video_format_detector::get_full_media_type() const
{
    return "video/raw";
}

detector::transport_type video_format_detector::get_transport_type() const
{
    return "RTP";
}
