#pragma once

#include <cmath>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
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

inline double FramePtsSeconds(const AVFrame *frame, const AVStream *stream)
{
	if (!frame || !stream) {
		return 0.0;
	}
	int64_t ts = (frame->best_effort_timestamp != AV_NOPTS_VALUE) ? frame->best_effort_timestamp
								      : frame->pts;
	if (ts == AV_NOPTS_VALUE) {
		return 0.0;
	}
	double sec = PtsToSeconds(ts, stream->time_base);
	if (stream->start_time != AV_NOPTS_VALUE && stream->start_time > 0) {
		sec -= PtsToSeconds(stream->start_time, stream->time_base);
	}
	return sec;
}

} // namespace overlay::ffmpeg
