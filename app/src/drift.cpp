#include "drift.h"

#include <algorithm>
#include <cmath>

// ------------------------------------------------------------------------------ PtsClock

PtsClock::PtsClock(double rate, double window_s, double span_s)
    : p_rate(rate), p_window(window_s), p_span(span_s)
{}

void PtsClock::reset()
{
	p_mins.clear();
	p_in_window = false;
}

void PtsClock::update(uint64_t position, double arrival)
{
	double d = arrival - double(position) / p_rate;
	if (!p_in_window) {
		if (p_mins.empty()) p_t0 = arrival;
		p_in_window = true;
		p_win_start = p_win_t = arrival;
		p_win_min = d;
		return;
	}
	if (d < p_win_min) {
		p_win_min = d;
		p_win_t = arrival;
	}
	if (arrival - p_win_start < p_window) return;
	p_mins.emplace_back(p_win_t - p_t0, p_win_min);
	while (p_mins.back().first - p_mins.front().first > p_span) p_mins.pop_front();
	p_win_start = p_win_t = arrival;
	p_win_min = d;
	fit();
}

void PtsClock::fit()
{
	// Least squares of delay over time, twice: the second pass without the windows more than
	// 3 sigma off the first line (a stall that lasted a whole window).
	double slope = 0, se = 0, sigma = INFINITY, a = 0;
	for (int pass = 0; pass < 2; pass++) {
		double n = 0, st = 0, sd = 0;
		auto keep = [&](const std::pair<double, double>& p) {
			return pass == 0 || std::fabs(p.second - (a + slope * p.first)) <= 3 * sigma;
		};
		for (auto& p : p_mins)
			if (keep(p)) {
				n++;
				st += p.first;
				sd += p.second;
			}
		if (n < 6) return;
		double mt = st / n, md = sd / n, sxx = 0, sxy = 0;
		for (auto& p : p_mins)
			if (keep(p)) {
				sxx += (p.first - mt) * (p.first - mt);
				sxy += (p.first - mt) * (p.second - md);
			}
		if (sxx <= 0) return;
		double b = sxy / sxx, ia = md - b * mt, rss = 0;
		for (auto& p : p_mins)
			if (keep(p)) rss += std::pow(p.second - (ia + b * p.first), 2);
		sigma = std::sqrt(rss / (n - 2)) + 1e-6;
		se = sigma / std::sqrt(sxx);
		slope = b;
		a = ia;
	}
	// The phone's samples arrive ever earlier when its clock is fast: the delay falls.
	double ppm = -slope * 1e6;
	p_error_ppm = se * 1e6;
	if (p_error_ppm < 20 && std::fabs(ppm) < 2000) {
		p_ppm = ppm;
		p_valid = true;
	}
}

// ------------------------------------------------------------------------------ RadioClock

namespace {
constexpr int kSettleWindows = 2;
}

RadioClock::RadioClock(double period_s, double max_ppm)
    : p_window(std::clamp(period_s / 30, 0.5, 10.0)), p_max(max_ppm), p_settle(kSettleWindows)
{
	// The backlog integrates the rate error (1 ppm moves it 1 µs a second): a PI loop around
	// it is a second-order system with this natural period, damping 0.7.
	double wn = 2 * M_PI / period_s;
	p_kp = 2 * 0.7 * wn * 1e6;
	p_ki = wn * wn * 1e6;
}

void RadioClock::update(double now, const ReadStats& s, double byte_rate)
{
	if (p_win_start < 0) p_win_start = now;
	p_win.gets += s.gets;
	p_win.waits += s.waits;
	p_win.discontinuities += s.discontinuities;
	p_win.backlog_sum += s.backlog_sum;
	p_win.since_put_sum += s.since_put_sum;
	if (now - p_win_start < p_window) return;
	end_window(byte_rate);
	p_win = {};
	p_win_start = now;
}

void RadioClock::end_window(double byte_rate)
{
	// Only a window of steady reads says anything about the radio's clock. No reads (paused),
	// a read that waited (the radio is ahead of us) or a cut backlog: settle again afterwards,
	// around wherever the backlog is then.
	if (p_win.gets == 0 || p_win.waits || p_win.discontinuities || byte_rate <= 0) {
		p_settle = kSettleWindows;
		p_error = 0;
		return;
	}
	double backlog = (p_win.backlog_sum / byte_rate + p_win.since_put_sum) / p_win.gets;
	if (p_settle > 0) {
		if (--p_settle == 0) p_setpoint = backlog;
		return;
	}
	// A falling backlog means the radio takes more than we make: speed up.
	p_error = backlog - p_setpoint;
	p_integral = std::clamp(p_integral - p_ki * p_error * p_window, -p_max, p_max);
}

double RadioClock::trim_ppm() const
{
	return std::clamp(p_integral - p_kp * p_error, -p_max, p_max);
}

// ------------------------------------------------------------------------------ Resampler

namespace {

double bessel_i0(double x)
{
	double sum = 1, term = 1;
	for (int k = 1; k < 30; k++) {
		term *= (x / (2 * k)) * (x / (2 * k));
		sum += term;
	}
	return sum;
}

}  // namespace

Resampler::Resampler(int channels)
    : p_ch(channels), p_coef((kPhases + 1) * kTaps), p_delta(kPhases * kTaps)
{
	// Cutoff at 0.91 of Nyquist (20 kHz at 44.1), Kaiser window: flat through the audio band
	// with about 70 dB of stopband. At ratios this close to 1 there is next to nothing to alias.
	const double fc = 0.91, beta = 7.0, half = kTaps / 2.0;
	for (int j = 0; j <= kPhases; j++) {
		float* row = &p_coef[j * kTaps];
		double frac = double(j) / kPhases, sum = 0;
		for (int k = 0; k < kTaps; k++) {
			double x = k - (half - 1) - frac;  // distance from the output instant, in frames
			double r = x / half;
			double w =
			    std::fabs(r) >= 1 ? 0 : bessel_i0(beta * std::sqrt(1 - r * r)) / bessel_i0(beta);
			double h = x == 0 ? fc : std::sin(M_PI * fc * x) / (M_PI * x);
			row[k] = float(h * w);
			sum += h * w;
		}
		for (int k = 0; k < kTaps; k++) row[k] = float(row[k] / sum);
	}
	for (int i = 0; i < kPhases * kTaps; i++) p_delta[i] = p_coef[i + kTaps] - p_coef[i];
	p_coef.resize(kPhases * kTaps);
	reset();
}

void Resampler::reset()
{
	p_hist.assign(size_t(kTaps / 2 - 1) * p_ch, 0.f);
	p_pos = kTaps / 2 - 1;
}

void Resampler::process(const int16_t* in, size_t frames, double ratio, std::vector<int16_t>& out)
{
	p_hist.insert(p_hist.end(), in, in + frames * p_ch);
	const long have = long(p_hist.size() / p_ch);
	const double step = 1.0 / ratio;
	auto clip = [](float v) { return int16_t(std::clamp(std::lrint(v), -32768L, 32767L)); };
	static_assert(kTaps % 4 == 0, "the loops below take taps four at a time");
	for (long i; (i = long(p_pos)) + kTaps / 2 < have; p_pos += step) {
		double fp = (p_pos - i) * kPhases;
		int j = int(fp);
		float a = float(fp - j);

		// The phase's coefficients, interpolated toward the next phase's, are made on the fly.
		// Four sums per channel instead of one: the ARM11's VFP takes 8 cycles per dependent
		// multiply-add, so one running sum would stall on every tap.
		const float* c = &p_coef[j * kTaps];
		const float* d = &p_delta[j * kTaps];
		const float* x = &p_hist[size_t(i - kTaps / 2 + 1) * p_ch];
		if (p_ch == 2) {
			float l0 = 0, l1 = 0, l2 = 0, l3 = 0, r0 = 0, r1 = 0, r2 = 0, r3 = 0;
			for (int k = 0; k < kTaps; k += 4, x += 8) {
				float c0 = c[k] + a * d[k], c1 = c[k + 1] + a * d[k + 1];
				float c2 = c[k + 2] + a * d[k + 2], c3 = c[k + 3] + a * d[k + 3];
				l0 += c0 * x[0];
				r0 += c0 * x[1];
				l1 += c1 * x[2];
				r1 += c1 * x[3];
				l2 += c2 * x[4];
				r2 += c2 * x[5];
				l3 += c3 * x[6];
				r3 += c3 * x[7];
			}
			out.push_back(clip((l0 + l1) + (l2 + l3)));
			out.push_back(clip((r0 + r1) + (r2 + r3)));
		} else {
			for (int ch = 0; ch < p_ch; ch++) {
				float v = 0;
				for (int k = 0; k < kTaps; k++) v += (c[k] + a * d[k]) * x[k * p_ch + ch];
				out.push_back(clip(v));
			}
		}
	}
	// Keep only the frames a later output still reaches back to.
	long first = long(p_pos) - kTaps / 2 + 1;
	if (first > 0) {
		p_hist.erase(p_hist.begin(), p_hist.begin() + first * p_ch);
		p_pos -= first;
	}
}
