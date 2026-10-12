// Simulates the touch front end's frame pacing (android/native/pacing.h)
// against screens of different refresh rates and sound devices that take
// their samples in blocks of different sizes, and the pacing it replaced
// (run frames while the sound queue is short), and counts what a player
// sees and hears: frames skipped, frames held on screen too long, and gaps
// in the sound.
//
//   c++ -O2 -std=c++17 -I android/native tools/pacing_sim.cpp -o pacing_sim && ./pacing_sim
#include "pacing.h"
#include <cstdio>
#include <random>

namespace {

const double RATE = 48000;

struct Scenario {
	const char *name;
	double hz;		// the screen's real refresh rate
	double reported;	// what the system says it is
	double fps;		// the NES's
	int block;		// samples the sound device takes at a time
	double frame_cost;	// seconds of work per emulated frame
	double spike;		// chance per refresh of a 20 ms hiccup (other apps, GC)
};

// the sound device: takes `block` samples every block/RATE seconds
struct Device {
	int block;
	double next = 0;
	double queue = 0;
	bool started = false;
	int underruns = 0;
	void advance(double t) {
		for (; next <= t; next += block / RATE) {
			if (queue >= block) queue -= block;
			else { if (started) underruns++; queue = 0; }
		}
	}
	void add(double n) { queue += n; if (queue >= block) started = true; }
};

struct Result { int skips = 0, holds = 0, underruns = 0; double lo = 1, hi = 1, speed = 0; };

Result simulate(const Scenario &s, bool new_pacing, double seconds) {
	std::mt19937 rng(1);
	std::uniform_real_distribution<double> jitter(0, 0.0015), chance(0, 1);
	Device dev{s.block};
	Pacer pacer;
	AudioRate rate;
	double frame_samples = RATE / s.fps, made_frac = 0;
	double target = s.block + 3 * frame_samples;
	double period = 1 / s.hz, t = 0, last = 0;
	long frame = 0, last_shown = 0;
	long vsync_last = 0;
	int held = 0, normal_hold = (int)std::ceil(s.hz / s.fps - 1e-9);
	Result r;
	auto make_frame = [&](void) {
		frame++;
		made_frac += frame_samples;
		double n = std::floor(made_frac);
		made_frac -= n;
		return n;
	};
	while (t < seconds) {
		double dt = t - last;
		last = t;
		dev.advance(t);
		int ran = 0;
		double made = 0;
		if (!new_pacing) {
			while (dev.queue < 4 * frame_samples && ran < 4) {
				dev.add(make_frame());
				ran++;
			}
		} else {
			int n = pacer.frames(dt, s.fps, s.reported);
			for (; ran < n; ran++) made += make_frame();
			if (dev.queue == 0) dev.add(target - frame_samples);	// (main.cpp: starting, or after a gap)
			double k = rate.ratio(made, dt, dev.queue, target, RATE);
			if (t > 10) {	// (once settled)
				r.lo = std::min(r.lo, k);
				r.hi = std::max(r.hi, k);
			}
			dev.add(made * k);
		}
		double done = t + ran * s.frame_cost + 0.001 + (chance(rng) < s.spike ? 0.020 : 0);
		dev.advance(done);
		long v = (long)std::floor(done / period) + 1;	// the refresh that shows it
		// what the refreshes since the last present showed
		for (long k = vsync_last + 1; k < v; k++) held++;
		if (frame > last_shown) {
			if (held + 1 > normal_hold) r.holds++;
			r.skips += (int)(frame - last_shown - 1);
			held = 0;
		} else
			held++;
		last_shown = frame;
		vsync_last = v;
		t = v * period + jitter(rng);
	}
	r.underruns = dev.underruns;
	r.speed = frame / t / s.fps;
	return r;
}

}

int main(void) {
	const double NTSC = 39375000.0 / 655171, PAL = 26601712.0 / 531960;
	const Scenario list[] = {
		{"60 Hz screen, 1024-sample sound blocks", 60, 60, NTSC, 1024, 0.004, 0},
		{"60 Hz screen, 256-sample sound blocks", 60, 60, NTSC, 256, 0.004, 0},
		{"60 Hz screen, 1920-sample sound blocks", 60, 60, NTSC, 1920, 0.004, 0},
		{"59.94 Hz screen (reported as 59)", 59.94, 59, NTSC, 1024, 0.004, 0},
		{"90 Hz screen", 90, 90, NTSC, 1024, 0.004, 0},
		{"120 Hz screen", 120, 120, NTSC, 1024, 0.004, 0},
		{"144 Hz screen", 144, 144, NTSC, 1024, 0.004, 0},
		{"PAL game, 60 Hz screen", 60, 60, PAL, 1024, 0.004, 0},
		{"60 Hz screen, a 20 ms hiccup every ~3 s", 60, 60, NTSC, 1024, 0.004, 0.005},
		{"60 Hz screen, slow phone (12 ms a frame)", 60, 60, NTSC, 1024, 0.012, 0},
		{"60 Hz screen, too slow (20 ms a frame)", 60, 60, NTSC, 1024, 0.020, 0},
	};
	const double seconds = 120;
	printf("per minute: frames skipped / frames held too long / sound gaps, then game speed   (%g s each)\n\n", seconds);
	printf("%-44s %-26s %-26s %s\n", "", "old (sound-paced)", "new (screen-paced)", "sound stretch");
	for (const Scenario &s : list) {
		Result o = simulate(s, false, seconds), n = simulate(s, true, seconds);
		double m = 60 / seconds;
		printf("%-44s %5.0f %5.0f %5.0f %4.0f%%    %5.0f %5.0f %5.0f %4.0f%%    %+.2f%% .. %+.2f%%\n", s.name,
		       o.skips * m, o.holds * m, o.underruns * m, o.speed * 100, n.skips * m, n.holds * m, n.underruns * m, n.speed * 100,
		       (n.lo - 1) * 100, (n.hi - 1) * 100);
	}
	return 0;
}
