#pragma once

#include "ebu/list/core/media/video_description.h"
#include "ebu/list/st2110/common_video_detector.h"
#include "ebu/list/st2110/d21/settings.h"
#include <cstdint>
#include <optional>
#include <vector>

namespace ebu_list::st2110::d20
{
    // Facts read from the RFC 4175 sample row data (SRD) headers and payload
    // of a ST 2110-20 stream. They give the pgroup, so the sampling and the
    // bit depth, without an SDP.
    struct pgroup_evidence
    {
        // Sum of the SRD lengths (bytes) and of the pixel distance to the next
        // SRD of the same line, over every SRD that has a next SRD on its line.
        int64_t srd_bytes  = 0;
        int64_t srd_pixels = 0;
        // Greatest common divisor of those pixel distances.
        int pixel_step_gcd = 0;
        // True when a line number is odd. RFC 4175 4:2:0 numbers only the
        // first line of each line pair.
        bool odd_line_seen = false;
        // A bounded sample of SRD payloads. Each one starts on a pgroup.
        std::vector<std::vector<uint8_t>> payload_samples;
    };

    struct sampling_and_depth
    {
        media::video::video_sampling sampling;
        int color_depth;
    };

    // Returns the sampling and the bit depth that the evidence supports, or
    // nothing when it supports no ST 2110-20 format.
    std::optional<sampling_and_depth> classify_pgroup(const pgroup_evidence& evidence);

    class line_data_analyzer
    {
      public:
        detector::status_description handle_data(const rtp::packet& packet);

        int max_line_number() const noexcept;
        bool is_field_based() const noexcept;
        const pgroup_evidence& evidence() const noexcept;

      private:
        int max_line_number_ = 0;
        int line_number_     = 0;
        bool is_field_based_ = false;

        pgroup_evidence evidence_;
        // The last SRD seen: its line, field, offset and length.
        int last_line_   = -1;
        int last_field_  = -1;
        int last_offset_ = -1;
        int last_length_ = 0;
        int srd_count_   = 0;
    };

    class video_format_detector : public sub_detector
    {
      public:
        video_format_detector();

        detector::status_description handle_data(const rtp::packet& packet) override;
        virtual details get_details() const override;
        virtual full_type get_full_media_type() const override;
        virtual transport_type get_transport_type() const override;
        inline virtual std::string get_kind() const noexcept override { return "video"; }

      private:
        common_video_detector detector_;
        line_data_analyzer line_analyzer_;
        packet_spacing_analyzer spacing_analyzer_;
    };
} // namespace ebu_list::st2110::d20
