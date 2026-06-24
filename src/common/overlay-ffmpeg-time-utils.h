#pragma once

#include <cmath>

extern "C" {
#include <libavutil/avutil.h>
#include <libavutil/rational.h>
}

namespace overlay::ffmpeg {

inline double PtsToSeconds(int64_t pts, AVRational time_base)
{
	if (pts == AV_NOPTS_VALUE || time_base.den <= 0) {
		return 0.0;
	}
	return static_cast<double>(pts) * av_q2d(time_base);
}

inline int64_t SecondsToTimestamp(double seconds, AVRational time_base)
{
	if (seconds <= 0.0 || time_base.den <= 0) {
		return 0;
	}
	int64_t us = static_cast<int64_t>(llround(seconds * static_cast<double>(AV_TIME_BASE)));
	return av_rescale_q(us, AVRational{1, AV_TIME_BASE}, time_base);
}

} // namespace overlay::ffmpeg
