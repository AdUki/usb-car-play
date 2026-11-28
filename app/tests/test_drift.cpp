// Drift correction, offline: the resampler's accuracy, the PTS clock against a jittery phone,
// and the radio loop against a radio whose crystal is off, simulated over an hour.

#include "drift.h"
#include "host.h"

#include <cmath>
#include <cstdlib>
#include <random>

namespace {

// A sine through the resampler at `ratio`, against the exact resampled sine: SNR in dB.
double resampler_snr(double freq, double ratio, size_t* out_frames = nullptr)
{
	const double fs = 44100, amp = 10000;
	const size_t n = 2 * 44100;
	std::vector<int16_t> in(n * 2), out;
	for (size_t i = 0; i < n; i++)
		in[2 * i] = in[2 * i + 1] = int16_t(lrint(amp * std::sin(2 * M_PI * freq * i / fs)));
	Resampler rs(2);
	for (size_t at = 0, k = 0; at < n; k++) {
		size_t step = std::min<size_t>(k % 2 ? 128 : 441, n - at);  // uneven calls, like packets
		rs.process(&in[2 * at], step, ratio, out);
		at += step;
	}
	if (out_frames) *out_frames = out.size() / 2;
	double sig = 0, err = 0;
	for (size_t m = 200; m + 200 < out.size() / 2; m++) {
		double ideal = amp * std::sin(2 * M_PI * freq * (m / ratio) / fs);
		sig += ideal * ideal;
		err += std::pow(out[2 * m] - ideal, 2) + std::pow(out[2 * m + 1] - ideal, 2);
	}
	return 10 * std::log10(sig / (err / 2));
}

void test_resampler()
{
	size_t frames = 0;
	if (getenv("DRIFT_VERBOSE"))
		fprintf(stderr, "resampler SNR: 1k %.1f dB, 1k@+800ppm %.1f, 12k %.1f, 17k %.1f\n",
		        resampler_snr(1000, 1.0), resampler_snr(1000, 1 + 800e-6),
		        resampler_snr(12000, 1 + 300e-6), resampler_snr(17000, 1 - 300e-6));
	CHECK(resampler_snr(1000, 1.0, &frames) > 75);
	CHECK(std::abs(long(frames) - long(2 * 44100 - Resampler::delay_frames())) <= 1);
	CHECK(resampler_snr(1000, 1 + 800e-6, &frames) > 75);
	CHECK(std::abs(double(frames) - 2 * 44100 * (1 + 800e-6)) < 2 * Resampler::delay_frames());
	CHECK(resampler_snr(1000, 1 - 800e-6) > 75);
	CHECK(resampler_snr(12000, 1 + 300e-6) > 75);
	CHECK(resampler_snr(17000, 1 - 300e-6) > 75);

	// A ratio that swings on every call: still a clean sine, no steps.
	Resampler rs(2);
	std::vector<int16_t> in(2 * 441), out;
	double phase = 0;
	for (int k = 0; k < 200; k++) {
		for (size_t i = 0; i < 441; i++) {
			in[2 * i] = in[2 * i + 1] = int16_t(lrint(10000 * std::sin(phase)));
			phase += 2 * M_PI * 1000 / 44100;
		}
		rs.process(in.data(), 441, k % 2 ? 1.001 : 0.999, out);
	}
	int jumps = 0;
	for (size_t m = 2 * 100; m + 2 < out.size(); m += 2)
		if (std::abs(out[m + 2] - out[m]) > 1500) jumps++;  // a 1 kHz sine moves <= 1425
	CHECK_EQ(jumps, 0);
}

// A phone at `ppm` sending 640-frame packets for `seconds`, over a link with Bluetooth-like
// delays: a few ms of jitter, now and then a retransmission burst of 100+ ms.
double pts_estimate(double ppm, double seconds, bool* valid)
{
	std::mt19937 rng(7);
	std::exponential_distribution<double> jitter(1 / 0.006);
	std::uniform_real_distribution<double> u(0, 1);
	PtsClock clock(44100);
	const double t0 = 5000;  // the Pi has been up for a while
	double burst = 0;
	for (uint64_t pos = 0; pos / 44100.0 < seconds; pos += 640) {
		double sent = t0 + pos / (44100 * (1 + ppm * 1e-6));
		if (u(rng) < 0.01) burst = 0.1 + 0.1 * u(rng);
		double arrival = sent + 0.012 + jitter(rng) + burst;
		burst = std::max(0.0, burst - 0.015);
		clock.update(pos, arrival);
	}
	*valid = clock.valid();
	return clock.ppm();
}

void test_pts_clock()
{
	for (double ppm : {250.0, -80.0, 0.0}) {
		bool valid = false;
		double est = pts_estimate(ppm, 300, &valid);
		if (getenv("DRIFT_VERBOSE")) fprintf(stderr, "phone %+.0f: est %+.2f\n", ppm, est);
		CHECK(valid);
		CHECK(std::fabs(est - ppm) < 5);
	}
	bool valid = true;
	pts_estimate(100, 8, &valid);  // too short to say
	CHECK(!valid);
}

// The radio loop against a simulated radio: a 72 KiB buffer it refills 18 KiB at a time,
// drained by a DAC `radio_ppm` off the Pi's clock. Returns the loop's final estimate and the
// largest backlog excursion (s) after it first locked.
double radio_sim(double radio_ppm, double seconds, double* excursion)
{
	const double byte_rate = 176400, dt = 0.02;
	const size_t chunk = 4608, read = 4 * chunk, cap = 4 * read;
	RadioClock loop(300, 1000);
	double queue = 0.4 * byte_rate, made = 0, buffer = cap, t = 0;
	double smooth = queue, setpoint = -1;  // the backlog averaged over 10 s: no read sawtooth
	*excursion = 0;
	for (; t < seconds; t += dt) {
		// The Pi makes whole chunks at its clock, trimmed by the loop.
		made += byte_rate * (1 + loop.trim_ppm() * 1e-6) * dt;
		while (made >= chunk) {
			queue += chunk;
			made -= chunk;
		}
		buffer -= byte_rate * (1 + radio_ppm * 1e-6) * dt;
		ReadStats s;
		while (buffer + read <= cap) {
			size_t got = 0;
			for (; got < read; got += chunk) {
				if (queue < chunk) {
					s.waits++;
					break;
				}
				queue -= chunk;
				s.gets++;
				s.backlog_sum += queue;
			}
			if (!got) break;
			buffer += got;
		}
		loop.update(t, s, byte_rate);
		smooth += (queue - smooth) * dt / 10;
		if (t >= 30 && setpoint < 0) setpoint = smooth / byte_rate;
		if (setpoint >= 0)
			*excursion = std::max(*excursion, std::fabs(smooth / byte_rate - setpoint));
	}
	return loop.ppm();
}

void test_radio_clock()
{
	for (double ppm : {150.0, -150.0, 0.0}) {
		double excursion = 0;
		double est = radio_sim(ppm, 3600, &excursion);
		if (getenv("DRIFT_VERBOSE"))
			fprintf(stderr, "radio %+.0f: est %+.1f excursion %.3f\n", ppm, est, excursion);
		CHECK(std::fabs(est - ppm) < 15);
		CHECK(excursion < 0.05);
	}
}

}  // namespace

// The resampler's cost on this CPU: the share of one core it needs for 44.1 kHz stereo.
void bench_resampler()
{
	std::vector<int16_t> in(2 * 640), out;
	for (size_t i = 0; i < in.size(); i++) in[i] = int16_t(lrint(10000 * std::sin(i * 0.07)));
	Resampler rs(2);
	const int packets = 44100 * 20 / 640;  // 20 s of audio in A2DP-sized pieces
	double t0 = now_s();
	for (int p = 0; p < packets; p++) {
		out.clear();
		rs.process(in.data(), 640, 1 + 120e-6, out);
	}
	double cpu = (now_s() - t0) / 20;
	printf("resampler: %.1f%% of a core for 44.1 kHz stereo\n", 100 * cpu);
}

void test_drift()
{
	test_resampler();
	test_pts_clock();
	test_radio_clock();
}
