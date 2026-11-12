#pragma once
// Bink movies on a screen that is not the movie's shape (src/native.c bink_fit_movie_viewport on the Direct3D 9 device): the movie's quad covers the whole viewport, so the viewport is made the
// movie's shape, centred (black bars). The draw is a movie's when it has textures of the movie's size (the luma plane) and of half that size (the chroma planes).
#include "natdraw11.h"

// native.c: the size of the movie being played (0 when none)
extern int native_nat_film_size(unsigned *w, unsigned *h);

// the viewport (x, y, w, h) of a draw made smaller to the movie's shape; 1 when it changed
static int nf_fit(float vp[4], unsigned mw, unsigned mh)
{
	const double ma = (double)mw / (double)mh, va = (double)vp[2] / (double)vp[3];
	if (va > ma + 0.0001) {
		const float w = (float)((double)vp[3] * ma + 0.5);
		if (w <= 0 || w > vp[2])
			return 0;
		vp[0] += (vp[2] - w) / 2;
		vp[2] = w;
		return 1;
	}
	if (va + 0.0001 < ma) {
		const float h = (float)((double)vp[2] / ma + 0.5);
		if (h <= 0 || h > vp[3])
			return 0;
		vp[1] += (vp[3] - h) / 2;
		vp[3] = h;
		return 1;
	}
	return 0;
}
