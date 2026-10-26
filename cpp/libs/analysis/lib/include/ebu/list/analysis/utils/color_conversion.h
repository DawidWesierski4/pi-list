#pragma once

#include "ebu/list/core/media/video/dimensions.h"
#include "ebu/list/core/media/video/sampling.h"
#include "ebu/list/core/memory/bimo.h"

namespace ebu_list::analysis
{
    // todo: generalize and move this to core

    bisect::bimo::oview from_ycbcr422_to_rgba(oview ycbcr422, media::video::video_dimensions dimensions);
    bisect::bimo::oview from_ycbcr422_to_uyvy(oview ycbcr422, media::video::video_dimensions dimensions);

    // Converts a ST 2110-20 frame of any sampling and bit depth to RGBA.
    // A 4:2:0 frame gives a gray image of its Y samples.
    bisect::bimo::oview to_rgba(oview frame, media::video::video_dimensions dimensions,
                                media::video::video_sampling sampling, int depth);
} // namespace ebu_list::analysis
