#include <algorithm>
#include "VapourSynth4.h"
#include "VSHelper4.h"

typedef struct {
	VSNode* node;
	int thr;
	int tmax;
	int tthr2;
	const VSVideoInfo* vi;
} CheckmateData;

template<int minimum, int maximum>
static __forceinline int static_clip(int val) {
	if (val > maximum) {
		return maximum;
	}
	if (val < minimum) {
		return minimum;
	}
	return val;
}

static __forceinline int clip(int val, int minimum, int maximum) {
	if (val > maximum) {
		return maximum;
	}
	if (val < minimum) {
		return minimum;
	}
	return val;
}

static inline void process_line_c(uint8_t* dstp, const uint8_t* srcp_p2, const uint8_t* srcp_p1, const uint8_t* srcp, const uint8_t* srcp_n1, const uint8_t* srcp_n2,
	ptrdiff_t src_pitch, ptrdiff_t src_p1_pitch, ptrdiff_t src_n1_pitch, int width, int thr, int tmax, int tthr2) {
	uint16_t tmax_multiplier = ((1 << 13) / tmax);
	for (int x = 0; x < width; ++x)
	{
		if (std::abs(srcp_p1[x] - srcp_n1[x]) < tthr2 && std::abs(srcp_p2[x] - srcp[x]) < tthr2 && std::abs(srcp[x] - srcp_n2[x]) < tthr2) {
			dstp[x] = (srcp_p1[x] + srcp[x] * 2 + srcp_n1[x]) >> 2;
		}
		else {
			int next_value = srcp[x] + srcp_n1[x];
			int prev_value = srcp[x] + srcp_p1[x];

			int x_left = x < 2 ? 0 : x - 2;
			int x_right = x > width - 3 ? width - 1 : x + 2;

			int current_column = srcp[x - src_pitch * 2] + srcp[x] * 2 + srcp[x + src_pitch * 2];
			int curr_value = (-srcp[x_left - src_pitch * 2] - srcp[x_right - src_pitch * 2]
				+ srcp[x_left] * 2 + srcp[x_right] * 2
				- srcp[x_left + src_pitch * 2] - srcp[x_right + src_pitch * 2]
				+ current_column * 2 + srcp[x] * 12) / 10;

			int nc = (srcp_n1[x - src_n1_pitch * 2] + srcp_n1[x] * 2 + srcp_n1[x + src_n1_pitch * 2]) - current_column;
			int pc = (srcp_p1[x - src_p1_pitch * 2] + srcp_p1[x] * 2 + srcp_p1[x + src_p1_pitch * 2]) - current_column;

			nc = thr + tmax - std::abs(nc);
			pc = thr + tmax - std::abs(pc);

			int next_weight = std::min(clip(nc, 0, tmax + 1) * tmax_multiplier, 8192);
			int prev_weight = std::min(clip(pc, 0, tmax + 1) * tmax_multiplier, 8192);

			int curr_weight = (1 << 14) - (next_weight + prev_weight);

			dstp[x] = static_clip<0, 255>((curr_weight * curr_value + prev_weight * prev_value + next_weight * next_value) >> 15);
		}
	}
}


static const VSFrame* VS_CC checkmateGetFrame(int n, int activationReason, void* instanceData, void** frameData, VSFrameContext* frameCtx, VSCore* core, const VSAPI* vsapi) {
	CheckmateData* d = (CheckmateData*)instanceData;

	if (activationReason == arInitial) {
		if (d->tthr2 > 0)
			vsapi->requestFrameFilter(std::max(0, n - 2), d->node, frameCtx);
		vsapi->requestFrameFilter(std::max(0, n - 1), d->node, frameCtx);
		vsapi->requestFrameFilter(n, d->node, frameCtx);
		vsapi->requestFrameFilter(std::min(n + 1, d->vi->numFrames - 1), d->node, frameCtx);
		if (d->tthr2 > 0)
			vsapi->requestFrameFilter(std::min(n + 2, d->vi->numFrames - 1), d->node, frameCtx);
	}
	else if (activationReason == arAllFramesReady) {
		const VSFrame* src_p1 = vsapi->getFrameFilter(std::max(0, n - 1), d->node, frameCtx);
		const VSFrame* src = vsapi->getFrameFilter(n, d->node, frameCtx);
		const VSFrame* src_n1 = vsapi->getFrameFilter(std::min(n + 1, d->vi->numFrames - 1), d->node, frameCtx);

		const VSFrame* src_p2 = nullptr;
		const VSFrame* src_n2 = nullptr;

		if (d->tthr2 > 0) {
			src_p2 = vsapi->getFrameFilter(std::max(0, n - 2), d->node, frameCtx);
			src_n2 = vsapi->getFrameFilter(std::min(n + 2, d->vi->numFrames - 1), d->node, frameCtx);
		}

		const VSVideoFormat* fi = vsapi->getVideoFrameFormat(src);
		int height = vsapi->getFrameHeight(src, 0);
		int width = vsapi->getFrameWidth(src, 0);

		VSFrame* dst = vsapi->newVideoFrame(fi, width, height, src, core);

		int plane;
		for (plane = 0; plane < fi->numPlanes; plane++) {
			const uint8_t* srcp_p1 = vsapi->getReadPtr(src_p1, plane);
			const uint8_t* srcp = vsapi->getReadPtr(src, plane);
			const uint8_t* srcp_n1 = vsapi->getReadPtr(src_n1, plane);
			ptrdiff_t src_p1_pitch = vsapi->getStride(src_p1, plane);
			ptrdiff_t src_pitch = vsapi->getStride(src, plane);
			ptrdiff_t src_n1_pitch = vsapi->getStride(src_n1, plane);
			uint8_t* dstp = vsapi->getWritePtr(dst, plane);
			ptrdiff_t dst_pitch = vsapi->getStride(dst, plane);

			const uint8_t* srcp_p2 = nullptr;
			const uint8_t* srcp_n2 = nullptr;

			if (d->tthr2 > 0) {
				srcp_p2 = vsapi->getReadPtr(src_p2, plane);
				srcp_n2 = vsapi->getReadPtr(src_n2, plane);
			}

			int h = vsapi->getFrameHeight(src, plane);
			int w = vsapi->getFrameWidth(src, plane);

			vsh::bitblt(dstp, vsapi->getStride(dst, plane), srcp, vsapi->getStride(src, plane), static_cast<size_t>(w) * fi->bytesPerSample, 2);

			srcp_p1 += src_p1_pitch * 2;
			srcp += src_pitch * 2;
			srcp_n1 += src_n1_pitch * 2;
			dstp += dst_pitch * 2;

			for (int y = 2; y < h - 2; ++y) {
				process_line_c(dstp, srcp_p2, srcp_p1, srcp, srcp_n1, srcp_n2, src_pitch, src_p1_pitch, src_n1_pitch, w, d->thr, d->tmax, d->tthr2);
				srcp_p1 += src_p1_pitch;
				srcp += src_pitch;
				srcp_n1 += src_n1_pitch;
				dstp += dst_pitch;
			}

			vsh::bitblt(dstp, vsapi->getStride(dst, plane), srcp, vsapi->getStride(src, plane), static_cast<size_t>(w) * fi->bytesPerSample, 2);
		}

		vsapi->freeFrame(src_p2);
		vsapi->freeFrame(src_p1);
		vsapi->freeFrame(src);
		vsapi->freeFrame(src_n1);
		vsapi->freeFrame(src_n2);

		return dst;
	}

	return NULL;
}

static void VS_CC checkmateFree(void* instanceData, VSCore* core, const VSAPI* vsapi) {
	CheckmateData* d = (CheckmateData*)instanceData;
	vsapi->freeNode(d->node);
	free(d);
}

static void VS_CC checkmateCreate(const VSMap* in, VSMap* out, void* userData, VSCore* core, const VSAPI* vsapi) {
	CheckmateData d;
	CheckmateData* data;
	int err;

	d.node = vsapi->mapGetNode(in, "clip", 0, 0);
	d.vi = vsapi->getVideoInfo(d.node);

	if (d.vi->format.sampleType != stInteger || d.vi->format.bitsPerSample != 8) {
		vsapi->mapSetError(out, "Checkmate: only 8bit integer input supported");
		vsapi->freeNode(d.node);
		return;
	}

	d.thr = vsapi->mapGetIntSaturated(in, "thr", 0, &err);
	if (err)
		d.thr = 12;

	d.tmax = vsapi->mapGetIntSaturated(in, "tmax", 0, &err);
	if (err)
		d.tmax = 12;

	d.tthr2 = vsapi->mapGetIntSaturated(in, "tthr2", 0, &err);
	if (err)
		d.tthr2 = 0;

	if (d.tmax <= 0 || d.tmax > 255) {
		vsapi->mapSetError(out, "Checkmate: tmax value should be in range [1;255]");
		vsapi->freeNode(d.node);
		return;
	}

	if (d.tthr2 < 0) {
		vsapi->mapSetError(out, "Checkmate: tthr2 should be non-negative");
		vsapi->freeNode(d.node);
		return;
	}

	data = (CheckmateData*)malloc(sizeof(d));
	*data = d;

	VSFilterDependency deps[] = { {d.node, rpGeneral} };
	vsapi->createVideoFilter(out, "Checkmate", data->vi, checkmateGetFrame, checkmateFree, fmParallel, deps, 1, data, core);
}

VS_EXTERNAL_API(void) VapourSynthPluginInit2(VSPlugin* plugin, const VSPLUGINAPI* vspapi) {
	vspapi->configPlugin("com.julek.checkmate", "checkmate", "Spatial and temporal dot crawl reducer", VS_MAKE_VERSION(1, 0), VAPOURSYNTH_API_VERSION, 0, plugin);
	vspapi->registerFunction("Checkmate",
							"clip:vnode;"
							"thr:int:opt;"
							"tmax:int:opt;"
							"tthr2:int:opt;",
							"clip:vnode;",
							checkmateCreate, NULL, plugin);
}