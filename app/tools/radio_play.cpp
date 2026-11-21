// radio-play: plays a WAV file from a mounted stick the way a head unit does, for tests through
// a real USB host stack (dummy_hcd on the board, or a PC with the board plugged in).
//
//   radio-play [--out pcm.raw] [--seconds N] [--resume S] FILE [FILE...]
//                                     play each FILE for N seconds (the first from S s in, as a
//                                     radio resuming where it was at power-off)
//   radio-play --analyze pcm.raw [--expect HZ]                   pitch, clicks, silent gaps
//
// Reads go through O_DIRECT in 32 KiB chunks into a 192 KiB buffer that a 44.1 kHz "decoder"
// drains in real time: no page cache, no readahead, like FatFs on a radio. The header's
// LIST/INFO title (wav_info = true) is printed, which is what the radio would display.

#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <unistd.h>
#include <vector>

static double now() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

static std::string wav_title(const uint8_t* h, size_t n, size_t& data_off) {
    std::string title;
    data_off = 44;
    for (size_t p = 12; p + 8 <= n;) {
        uint32_t len = h[p + 4] | h[p + 5] << 8 | h[p + 6] << 16 | uint32_t(h[p + 7]) << 24;
        if (!memcmp(h + p, "data", 4)) { data_off = p + 8; break; }
        if (!memcmp(h + p, "LIST", 4) && p + 12 <= n && !memcmp(h + p + 8, "INFO", 4)) {
            for (size_t q = p + 12; q + 8 <= p + 8 + len && q + 8 <= n;) {
                uint32_t l = h[q + 4] | h[q + 5] << 8;
                std::string v(reinterpret_cast<const char*>(h + q + 8), strnlen(reinterpret_cast<const char*>(h + q + 8), l));
                if (!memcmp(h + q, "IART", 4)) title = v + (title.empty() ? "" : " - " + title);
                if (!memcmp(h + q, "INAM", 4)) title += (title.empty() ? "" : " - ") + v;
                q += 8 + l + (l & 1);
            }
        }
        p += 8 + len + (len & 1);
    }
    return title;
}

static int play(const std::vector<std::string>& files, double seconds, double resume, FILE* out) {
    const size_t chunk = 32768, cap = 196608, start = 65536;
    void* raw = nullptr;
    if (posix_memalign(&raw, 4096, chunk)) return 1;
    uint8_t* io = static_cast<uint8_t*>(raw);
    int underruns = 0;
    double max_read = 0;
    for (auto& path : files) {
        int fd = open(path.c_str(), O_RDONLY | O_DIRECT);
        if (fd < 0) { perror(path.c_str()); return 1; }
        double t_open = now();
        if (pread(fd, io, 4096, 0) != 4096) { perror("header"); return 1; }
        size_t data_off;
        std::string title = wav_title(io, 4096, data_off);
        printf("%s: opened in %.2f s, title \"%s\"\n", path.c_str(), now() - t_open, title.c_str());
        fflush(stdout);
        std::vector<uint8_t> buf;
        off_t pos = 0;
        size_t skip = data_off;
        if (resume > 0 && &path == &files[0]) {
            // A resume: straight from the header to the remembered position, 4 KiB aligned.
            off_t at = off_t(data_off + uint64_t(resume * 44100) * 4);
            pos = at - at % 4096;
            skip = size_t(std::max<off_t>(0, off_t(data_off) - pos));
        }
        bool playing = false;
        double t0 = now(), t_last = 0, frac = 0;
        bool eof = false;   // then play out what is buffered, as a radio does, before the next file
        while (now() - t0 < seconds && !(eof && buf.size() < 4)) {
            if (!eof && buf.size() + chunk <= cap) {
                double r0 = now();
                ssize_t n = pread(fd, io, chunk, pos);
                max_read = std::max(max_read, now() - r0);
                if (n <= 0) {
                    eof = true;
                    playing = true;
                    continue;
                }
                pos += n;
                size_t s = std::min<size_t>(skip, n);
                skip -= s;
                buf.insert(buf.end(), io + s, io + n);
            } else {
                usleep(5000);
            }
            double t = now();
            if (!playing) {
                if (buf.size() >= start) { playing = true; t_last = t; }
                continue;
            }
            frac += (t - t_last) * 44100;
            t_last = t;
            size_t frames = size_t(frac);
            frac -= frames;
            if (frames * 4 > buf.size()) {
                if (!eof) underruns++;
                frames = buf.size() / 4;
            }
            if (out) fwrite(buf.data(), 4, frames, out);
            buf.erase(buf.begin(), buf.begin() + frames * 4);
        }
        close(fd);
    }
    printf("underruns %d, slowest read %.3f s\n", underruns, max_read);
    free(raw);
    return underruns ? 2 : 0;
}

static int analyze(const char* path, double expect) {
    FILE* f = fopen(path, "rb");
    if (!f) { perror(path); return 1; }
    std::vector<int16_t> l;
    int16_t s[2];
    while (fread(s, 2, 2, f) == 2) l.push_back(s[0]);
    fclose(f);
    // Pitch per second, clicks (jumps no sine below 2 kHz at this level makes), silent runs.
    int peak = 0;
    for (int16_t v : l) peak = std::max(peak, std::abs(int(v)));
    int clicks = 0, gaps = 0;
    size_t silent = 0;
    double max_jump = 2 * M_PI * (expect > 0 ? expect * 1.5 : 2000) / 44100 * peak + 200;
    for (size_t k = 1; k < l.size(); k++) {
        if (std::abs(int(l[k]) - int(l[k - 1])) > max_jump) clicks++;
        if (std::abs(int(l[k])) < 50) silent++;
        else {
            if (silent > 441) gaps++;   // > 10 ms of silence inside the tone
            silent = 0;
        }
    }
    printf("%.1f s, peak %d, clicks %d, silent gaps %d\n", l.size() / 44100.0, peak, clicks, gaps);
    for (size_t a = 0; a + 44100 <= l.size(); a += 44100 * 5) {
        int z = 0;
        for (size_t k = a + 1; k < a + 44100; k++) if ((l[k - 1] < 0) != (l[k] < 0)) z++;
        printf("  t=%3zus  %.0f Hz\n", a / 44100, z / 2.0);
    }
    return 0;
}

int main(int argc, char** argv) {
    std::vector<std::string> files;
    double seconds = 10, expect = 0, resume = 0;
    const char *out_path = nullptr, *analyze_path = nullptr;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--seconds" && i + 1 < argc) seconds = atof(argv[++i]);
        else if (a == "--out" && i + 1 < argc) out_path = argv[++i];
        else if (a == "--analyze" && i + 1 < argc) analyze_path = argv[++i];
        else if (a == "--expect" && i + 1 < argc) expect = atof(argv[++i]);
        else if (a == "--resume" && i + 1 < argc) resume = atof(argv[++i]);
        else files.push_back(a);
    }
    if (analyze_path) return analyze(analyze_path, expect);
    if (files.empty()) {
        fprintf(stderr, "usage: radio-play [--out pcm.raw] [--seconds N] FILE...\n"
                        "       radio-play --analyze pcm.raw [--expect HZ]\n");
        return 2;
    }
    FILE* out = out_path ? fopen(out_path, "wb") : nullptr;
    int rc = play(files, seconds, resume, out);
    if (out) fclose(out);
    return rc;
}
