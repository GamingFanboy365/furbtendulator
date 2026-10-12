// Frame pacing and sound rate control for the touch front end (main.cpp),
// kept free of SDL so tools/pacing_sim.cpp can test it against simulated
// screens and sound devices.
//
// The screen is the clock: it refreshes at its own rate (60 Hz, or 90, 120,
// 144 on newer phones) and every refresh shows the newest frame.  The NES
// makes 60.0988 frames a second (PAL and Dendy 50.007).  When the two are
// within 2% of each other, or of a whole multiple, every frame is shown for
// the same number of refreshes -- one at 60 Hz, two at 120 -- and the game
// runs at the screen's speed (0.16% slow on a 60.00 Hz screen, which nobody
// can see).  Otherwise frames are spread as evenly as the two rates allow.
//
// The sound cannot be the clock as well: the sound device takes its samples
// in blocks (1024 or more on many phones), so pacing by the sound queue runs
// two frames in one refresh and none in the next, and the picture skips.  So
// the sound follows the picture instead, stretched or squeezed by up to 2%
// (AudioRate) to keep the sound queue at a steady level.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstddef>
#include <vector>

class Pacer {
public:
	// start over: after a pause, a fast-forward, a new game
	void reset(void) { acc = 0; racc = 0; nhist = 0; pos = 0; missed = 0; }

	// One call per presented refresh.  dt: seconds since the last call.
	// hz: the screen's refresh rate as the system reports it (0: unknown).
	// Returns how many frames to run now (0..4).
	int frames(double dt, double fps, double hz) {
		if (dt < 0) dt = 0;
		if (dt > 0.25) {	// the app was stalled or in the background: don't try to catch up
			reset();
			dt = 1 / fps;
		}
		measure(dt);
		double reported = hz > 1 ? 1 / hz : 0, measured = median();
		// the screen's refresh period: as reported, or as measured when the
		// report is a rounded figure (59 for 59.94) or missing
		period = reported ? reported : 1 / 60.0;
		if (measured > 0 && (!reported || std::fabs(measured / reported - 1) < 0.05)) period = measured;
		// presents that don't wait for the screen (a broken vsync): pace by
		// the time alone
		if (nhist >= 8 && measured < period * 0.5) {
			lock = 0;
			acc += dt * fps;
			return take();
		}
		// refreshes since the last call: 1, or 2 or more when one was missed.
		// racc is this present's offset from the refreshes expected, pulled a
		// little towards zero each time: it follows the presents' timing but
		// not their jitter, and a period that is a bit off never adds up to a
		// skipped or doubled frame.
		racc += dt / period;
		int r = std::max(0, (int)std::floor(racc + 0.5));
		racc = (racc - r) * 0.9;
		// one missed refresh now and then (the system was busy): show the
		// next frame late rather than skip one to catch up.  Missing them all
		// the time (a phone too slow for the game) is caught up by skipping.
		missed = missed << 1 | (r > 1);
		if (r > 1 && !(missed & 0xFFFE)) r = 1;
		double ratio = fps * period;		// frames per refresh
		int n = (int)std::lround(1 / ratio);	// refreshes per frame
		lock = n >= 1 && n <= 4 && std::fabs(ratio * n - 1) < 0.02 ? n : 0;
		acc += r * (lock ? 1.0 / lock : ratio);
		return take();
	}
	double refresh_hz(void) const { return 1 / period; }
	int locked(void) const { return lock; }	// refreshes per frame, or 0: not locked

private:
	double acc = 0, racc = 0, period = 1 / 60.0;
	int lock = 0;
	unsigned missed = 0;	// one bit per present, newest lowest: missed a refresh
	double hist[32];
	int nhist = 0, pos = 0;

	int take(void) {
		int run = (int)std::floor(acc + 1e-9);
		if (run > 4) {		// too far behind (a slow device): skip ahead
			run = 4;
			acc = 0;
		} else
			acc -= run;
		return run;
	}
	void measure(double dt) {
		if (dt <= 0) return;
		hist[pos] = dt;
		pos = (pos + 1) % 32;
		if (nhist < 32) nhist++;
	}
	double median(void) const {
		if (nhist < 8) return 0;
		double t[32];
		std::copy(hist, hist + nhist, t);
		std::nth_element(t, t + nhist / 2, t + nhist);
		return t[nhist / 2];
	}
};

// How much to stretch the sound so the queue stays near its target.
class AudioRate {
public:
	void reset(void) { made = 0; secs = 0; level = -1; }
	// made_now: samples the emulation made since the last call, dt: the
	// seconds that took, queued: samples waiting in the sound queue now.
	// Returns output samples per input sample.
	double ratio(double made_now, double dt, double queued, double target, double rate) {
		// the long-run ratio: samples the device plays over samples made,
		// over the last 8 seconds or so
		const double keep = std::exp(-dt / 8);
		made = made * keep + made_now;
		secs = secs * keep + dt;
		double base = made > rate * 0.1 ? std::clamp(rate * secs / made, 0.98, 1.02) : 1;
		// and a gentle pull towards the target level (smoothed: the device
		// takes its samples in blocks, so the level jumps up and down)
		level = level < 0 ? queued : level + (queued - level) * 0.05;
		double err = std::clamp((target - level) / target, -1.0, 1.0);
		return base * (1 + 0.01 * err);
	}
private:
	double made = 0, secs = 0, level = -1;
};

// Linear interpolation from one rate to a slightly different one, carried
// across calls.
class Resampler {
public:
	void reset(void) { pos = -1; prev = 0; }
	void run(const int16_t *in, size_t n, double ratio, std::vector<int16_t> &out) {
		if (!n) return;
		double step = 1 / ratio, p = pos;
		while (p < (double)n - 1) {
			int i = (int)std::floor(p);
			double f = p - i, a = i < 0 ? prev : in[i], b = in[i + 1];
			out.push_back((int16_t)std::lrint(a + (b - a) * f));
			p += step;
		}
		pos = p - (double)n;
		prev = in[n - 1];
	}
private:
	double pos = -1;	// next output's position, in input samples from this call's first (-1: prev)
	int16_t prev = 0;
};
