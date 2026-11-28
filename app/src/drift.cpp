#include "drift.h"

#include <algorithm>
#include <cmath>

// ------------------------------------------------------------------------------ PtsClock

PtsClock::PtsClock(double rate, double window_s, double span_s)
    : rate_(rate), window_(window_s), span_(span_s)
{}

void PtsClock::reset()
{
	mins_.clear();
	in_window_ = false;
}

void PtsClock::update(uint64_t position, double arrival)
{
	double d = arrival - double(position) / rate_;
	if (!in_window_) {
		if (mins_.empty()) t0_ = arrival;
		in_window_ = true;
		win_start_ = win_t_ = arrival;
		win_min_ = d;
		return;
	}
	if (d < win_min_) {
		win_min_ = d;
		win_t_ = arrival;
	}
	if (arrival - win_start_ < window_) return;
	mins_.emplace_back(win_t_ - t0_, win_min_);
	while (mins_.back().first - mins_.front().first > span_) mins_.pop_front();
	win_start_ = win_t_ = arrival;
	win_min_ = d;
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
		for (auto& p : mins_)
			if (keep(p)) {
				n++;
				st += p.first;
				sd += p.second;
			}
		if (n < 6) return;
		double mt = st / n, md = sd / n, sxx = 0, sxy = 0;
		for (auto& p : mins_)
			if (keep(p)) {
				sxx += (p.first - mt) * (p.first - mt);
				sxy += (p.first - mt) * (p.second - md);
			}
		if (sxx <= 0) return;
		double b = sxy / sxx, ia = md - b * mt, rss = 0;
		for (auto& p : mins_)
			if (keep(p)) rss += std::pow(p.second - (ia + b * p.first), 2);
		sigma = std::sqrt(rss / (n - 2)) + 1e-6;
		se = sigma / std::sqrt(sxx);
		slope = b;
		a = ia;
	}
	// The phone's samples arrive ever earlier when its clock is fast: the delay falls.
	double ppm = -slope * 1e6;
	error_ppm_ = se * 1e6;
	if (error_ppm_ < 20 && std::fabs(ppm) < 2000) {
		ppm_ = ppm;
		valid_ = true;
	}
}

// ------------------------------------------------------------------------------ RadioClock

namespace {
constexpr int kSettleWindows = 2;
}

RadioClock::RadioClock(double period_s, double max_ppm)
    : window_(std::clamp(period_s / 30, 0.5, 10.0)), max_(max_ppm), settle_(kSettleWindows)
{
	// The backlog integrates the rate error (1 ppm moves it 1 µs a second): a PI loop around
	// it is a second-order system with this natural period, damping 0.7.
	double wn = 2 * M_PI / period_s;
	kp_ = 2 * 0.7 * wn * 1e6;
	ki_ = wn * wn * 1e6;
}

void RadioClock::update(double now, const ReadStats& s, double byte_rate)
{
	if (win_start_ < 0) win_start_ = now;
	win_.gets += s.gets;
	win_.waits += s.waits;
	win_.discontinuities += s.discontinuities;
	win_.backlog_sum += s.backlog_sum;
	win_.since_put_sum += s.since_put_sum;
	if (now - win_start_ < window_) return;
	end_window(byte_rate);
	win_ = {};
	win_start_ = now;
}

void RadioClock::end_window(double byte_rate)
{
	// Only a window of steady reads says anything about the radio's clock. No reads (paused),
	// a read that waited (the radio is ahead of us) or a cut backlog: settle again afterwards,
	// around wherever the backlog is then.
	if (win_.gets == 0 || win_.waits || win_.discontinuities || byte_rate <= 0) {
		settle_ = kSettleWindows;
		error_ = 0;
		return;
	}
	double backlog = (win_.backlog_sum / byte_rate + win_.since_put_sum) / win_.gets;
	if (settle_ > 0) {
		if (--settle_ == 0) setpoint_ = backlog;
		return;
	}
	// A falling backlog means the radio takes more than we make: speed up.
	error_ = backlog - setpoint_;
	integral_ = std::clamp(integral_ - ki_ * error_ * window_, -max_, max_);
}

double RadioClock::trim_ppm() const
{
	return std::clamp(integral_ - kp_ * error_, -max_, max_);
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
    : ch_(channels), coef_((kPhases + 1) * kTaps), delta_(kPhases * kTaps)
{
	// Cutoff at 0.91 of Nyquist (20 kHz at 44.1), Kaiser window: flat through the audio band
	// with about 70 dB of stopband. At ratios this close to 1 there is next to nothing to alias.
	const double fc = 0.91, beta = 7.0, half = kTaps / 2.0;
	for (int j = 0; j <= kPhases; j++) {
		float* row = &coef_[j * kTaps];
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
	for (int i = 0; i < kPhases * kTaps; i++) delta_[i] = coef_[i + kTaps] - coef_[i];
	coef_.resize(kPhases * kTaps);
	reset();
}

void Resampler::reset()
{
	hist_.assign(size_t(kTaps / 2 - 1) * ch_, 0.f);
	pos_ = kTaps / 2 - 1;
}

void Resampler::process(const int16_t* in, size_t frames, double ratio, std::vector<int16_t>& out)
{
	hist_.insert(hist_.end(), in, in + frames * ch_);
	const long have = long(hist_.size() / ch_);
	const double step = 1.0 / ratio;
	auto clip = [](float v) { return int16_t(std::clamp(std::lrint(v), -32768L, 32767L)); };
	static_assert(kTaps % 4 == 0, "the loops below take taps four at a time");
	for (long i; (i = long(pos_)) + kTaps / 2 < have; pos_ += step) {
		double fp = (pos_ - i) * kPhases;
		int j = int(fp);
		float a = float(fp - j);

		// The phase's coefficients, interpolated toward the next phase's, are made on the fly.
		// Four sums per channel instead of one: the ARM11's VFP takes 8 cycles per dependent
		// multiply-add, so one running sum would stall on every tap.
		const float* c = &coef_[j * kTaps];
		const float* d = &delta_[j * kTaps];
		const float* x = &hist_[size_t(i - kTaps / 2 + 1) * ch_];
		if (ch_ == 2) {
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
			for (int ch = 0; ch < ch_; ch++) {
				float v = 0;
				for (int k = 0; k < kTaps; k++) v += (c[k] + a * d[k]) * x[k * ch_ + ch];
				out.push_back(clip(v));
			}
		}
	}
	// Keep only the frames a later output still reaches back to.
	long first = long(pos_) - kTaps / 2 + 1;
	if (first > 0) {
		hist_.erase(hist_.begin(), hist_.begin() + first * ch_);
		pos_ -= first;
	}
}
