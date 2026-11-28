// Clock drift between the phone and the radio, and its correction.
//
// Three clocks are involved: the phone's (it sends 44.1 kHz by its own crystal), the Pi's
// (CLOCK_MONOTONIC) and the radio's (its DAC drains the files at 44.1 kHz by its crystal). They
// differ by tens to hundreds of ppm, and 100 ppm adds up to 0.36 s an hour. Uncorrected, that
// either drains the radio's cushion (an underrun) or piles up behind it (a skip).
//
// A sink with its own DAC would tune its audio PLL to the source. btdisk has no audio clock to
// tune, because the DAC is in the radio. So it corrects with an ASRC, steered by two loops that
// both measure against the Pi's clock:
//   PtsClock    the phone's rate, from the A2DP packets' RTP timestamps (PTS) and arrival times
//   RadioClock  the radio's rate, from the backlog its reads leave behind in the chunk queue
// The Resampler converts the phone's samples to the radio's rate (radio / phone).
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

// The phone's sample clock against the Pi's. Each packet gives (position, arrival): its first
// sample's place on the phone's timeline and when it arrived. Arrival jitter only ever adds
// delay, so the lower envelope of (arrival - position / rate) is the clean line. Its slope is
// the drift, fitted by least squares over the windows' minima.
class PtsClock {
public:
    explicit PtsClock(double rate, double window_s = 2, double span_s = 600);
    void reset();                                  // a new stream: keeps the last estimate
    void update(uint64_t position, double arrival);
    double ppm() const { return ppm_; }            // + = the phone's clock runs fast
    bool valid() const { return valid_; }

private:
    void fit();
    double rate_, window_, span_;
    bool in_window_ = false;
    double win_start_ = 0, win_min_ = 0, win_t_ = 0, t0_ = 0;
    std::deque<std::pair<double, double>> mins_;   // (time, minimum delay) per window
    double ppm_ = 0, error_ppm_ = 0;               // error: the last fit's standard error
    bool valid_ = false;
};

// What the radio's reads did since the last look, collected by the chunk queue (stream.h).
struct ReadStats {
    uint64_t gets = 0;         // chunks the radio took
    uint64_t waits = 0;        // of those, how many found the queue empty
    uint64_t resets = 0;       // the backlog was cut (a new file, a skip to live)
    double backlog_sum = 0;    // queue bytes left after each get, summed
    // Seconds since the producer's last chunk, at each get, summed: the part of a chunk already
    // made. With it the backlog is measured in time, not in steps of a whole chunk (26 ms).
    double since_put_sum = 0;
};

// The radio's clock against the Pi's. While the radio paces its own reads (its buffer is full
// and every read finds audio waiting), the backlog left after its reads only moves when the two
// rates differ. A PI loop holds that backlog where it settled by trimming the output rate. When
// the queue runs dry, the radio is waiting for us and its clock can't be seen: the loop holds
// its last trim.
class RadioClock {
public:
    explicit RadioClock(double period_s = 300, double max_ppm = 1000);
    void update(double now, const ReadStats& s, double byte_rate);
    double ppm() const { return integral_; }       // the radio's clock, + = fast
    double trim_ppm() const;                       // to apply: the clock + the backlog's error
    bool locked() const { return settle_ == 0; }

private:
    void end_window(double byte_rate);
    double window_, kp_, ki_, max_;
    double win_start_ = -1;
    ReadStats win_;
    int settle_;
    double setpoint_ = 0, error_ = 0, integral_ = 0;
};

// An asynchronous sample rate converter for ratios close to 1: a windowed-sinc FIR, polyphase
// with linear interpolation between phases, so any ratio (and a ratio that changes from one
// call to the next) works without clicks.
class Resampler {
public:
    explicit Resampler(int channels);
    // Converts `frames` interleaved s16 frames at `ratio` output frames per input frame, and
    // appends the result to out.
    void process(const int16_t* in, size_t frames, double ratio, std::vector<int16_t>& out);
    void reset();
    static constexpr int delay_frames() { return kTaps / 2; }   // input to output

private:
    static constexpr int kTaps = 48;
    static constexpr int kPhases = 256;
    int ch_;
    std::vector<float> coef_;                      // kPhases rows of kTaps
    std::vector<float> delta_;                     // each row's step to the next phase's
    std::vector<float> hist_;                      // input frames still needed, interleaved
    double pos_;                                   // input frame of the next output frame
};
