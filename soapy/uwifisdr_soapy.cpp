/*
    This file is part of Kismet

    Kismet is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    Kismet is distributed in the hope that it will be useful,
      but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Kismet; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/

/* SoapySDR module for the USB Wi-Fi I/Q adapters (driver=uwifisdr): one RX channel, tuned
 * on the chip's 5 MHz raster ("RF") with a digital shift ("BB") for the rest, sample rate
 * set by the channel width */

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <SoapySDR/Device.hpp>
#include <SoapySDR/Formats.hpp>
#include <SoapySDR/Logger.hpp>
#include <SoapySDR/Registry.hpp>

#include "iqdev.h"
#include "iqout.h"
#include "iqradio.h"

#ifndef FIRMWAREDIR
#define FIRMWAREDIR "/usr/local/share/uwifi/firmware"
#endif

namespace {

constexpr size_t max_adapters = 16;
/* Samples per read from the radio */
constexpr size_t chunk_len = 16384;
/* About 50 ms at the widest rate, 200 ms at 20 MHz */
constexpr size_t ring_len = size_t(1) << 22;
constexpr double default_hz = 2437e6;
constexpr unsigned int widths[] = { 20, 40, 80, 160 };

/* The process's one USB layer; firmware directories only count on the first call */
bool usb_init(const std::string &fw_dir) {
    static std::once_flag once;
    static bool ok;

    std::call_once(once, [&] {
        std::vector<std::string> dirs;
        const char *env = std::getenv("UWIFI_FIRMWARE");

        if (!fw_dir.empty())
            dirs.push_back(fw_dir);
        if (env != nullptr && *env != 0)
            dirs.push_back(env);
        dirs.insert(dirs.end(), { FIRMWAREDIR, "/usr/local/share/kismet/firmware", "/usr/share/kismet/firmware" });

        /* The USB layer keeps the pointers */
        static std::vector<std::string> keep;
        static std::vector<const char *> paths;
        keep = dirs;
        for (const auto &d : keep)
            paths.push_back(d.c_str());

        ok = iqdev_init(paths.data(), paths.size(), [](void *, int level, const char *msg) {
            SoapySDR::logf(level <= 3 ? SOAPY_SDR_ERROR : level == 4 ? SOAPY_SDR_WARNING :
                    level <= 6 ? SOAPY_SDR_INFO : SOAPY_SDR_DEBUG, "uwifisdr: %s", msg);
        }, nullptr, std::getenv("UWIFI_VERBOSE") != nullptr ? 7 : 4) >= 0;
    });
    return ok;
}

std::vector<iqdev_info> list_adapters() {
    std::vector<iqdev_info> list(max_adapters);

    list.resize(size_t(std::max(0, iqdev_list(list.data(), int(list.size())))));
    return list;
}

std::string arg(const SoapySDR::Kwargs &args, const char *key) {
    auto i = args.find(key);
    return i == args.end() ? std::string() : i->second;
}

double rate_of(enum iqdev_chip chip, unsigned int bw) {
    return bw * IQRADIO_RATE_UNIT * (chip == IQDEV_MT7925 ? 2 : 1);
}

class UWifiSDR : public SoapySDR::Device {
public:
    explicit UWifiSDR(const SoapySDR::Kwargs &args) {
        std::string name = arg(args, "device");
        char err[256];

        if (!usb_init(arg(args, "firmware")))
            throw std::runtime_error("uwifisdr: unable to start the USB layer");
        if (name.empty()) {
            auto list = list_adapters();
            if (list.empty())
                throw std::runtime_error("uwifisdr: no I/Q adapters found");
            name = list[0].name;
        }
        for (const auto &a : list_adapters())
            if (name == a.name)
                info_ = a;
        if (info_.name[0] == 0)
            throw std::runtime_error("uwifisdr: no adapter " + name);

        iqdev *d = iqdev_open(info_.name, err, sizeof(err));
        if (d == nullptr)
            throw std::runtime_error(std::string("uwifisdr: ") + info_.name + ": " + err);
        if (!iqdev_superspeed(d))
            SoapySDR::logf(SOAPY_SDR_WARNING, "uwifisdr: %s isn't on a USB 3 port; most samples will be lost",
                    info_.name);
        const uint8_t *m = iqdev_mac(d);
        char mac[18];
        snprintf(mac, sizeof(mac), "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
        mac_ = mac;

        /* Takes the adapter, and closes it on failure */
        if ((radio_ = iqradio_new(d)) == nullptr)
            throw std::runtime_error("uwifisdr: out of memory");
        try {
            check(iqradio_set_gain(radio_, IQRADIO_GAIN_DEFAULT), "setting the gain");
            retune(default_hz);
        } catch (...) {
            iqradio_free(radio_);
            throw;
        }
        ring_.resize(ring_len);
        reader_ = std::thread(&UWifiSDR::read_loop, this);
    }

    ~UWifiSDR() override {
        {
            std::lock_guard<std::mutex> lk(ring_mtx_);
            quit_ = true;
        }
        run_cv_.notify_all();
        reader_.join();
        iqradio_free(radio_);
    }

    std::string getDriverKey() const override { return "uwifisdr"; }
    std::string getHardwareKey() const override { return iqdev_chip_name(info_.chip); }

    SoapySDR::Kwargs getHardwareInfo() const override {
        return { { "device", info_.name }, { "usb", info_.hardware }, { "mac", mac_ } };
    }

    size_t getNumChannels(const int dir) const override { return dir == SOAPY_SDR_RX ? 1 : 0; }

    std::vector<std::string> listAntennas(const int, const size_t) const override { return { "RX" }; }
    std::string getAntenna(const int, const size_t) const override { return "RX"; }

    /* Streams */

    std::vector<std::string> getStreamFormats(const int, const size_t) const override {
        return { SOAPY_SDR_CF32, SOAPY_SDR_CS16, SOAPY_SDR_CS8, SOAPY_SDR_CU8 };
    }

    std::string getNativeStreamFormat(const int, const size_t, double &full_scale) const override {
        full_scale = 1.0;
        return SOAPY_SDR_CF32;
    }

    SoapySDR::Stream *setupStream(const int dir, const std::string &format, const std::vector<size_t> &channels,
            const SoapySDR::Kwargs &) override {
        iqout_format fmt;

        if (dir != SOAPY_SDR_RX)
            throw std::runtime_error("uwifisdr: receive only");
        if (channels.size() > 1 || (channels.size() == 1 && channels[0] != 0))
            throw std::runtime_error("uwifisdr: one channel, 0");
        if (format == SOAPY_SDR_CF32)
            fmt = IQOUT_CF32;
        else if (format == SOAPY_SDR_CS16)
            fmt = IQOUT_CS16;
        else if (format == SOAPY_SDR_CS8)
            fmt = IQOUT_CS8;
        else if (format == SOAPY_SDR_CU8)
            fmt = IQOUT_CU8;
        else
            throw std::runtime_error("uwifisdr: unsupported format " + format);
        std::lock_guard<std::mutex> lk(ring_mtx_);
        if (stream_ != nullptr)
            throw std::runtime_error("uwifisdr: the stream is already set up");
        stream_ = new Stream{ fmt };
        return reinterpret_cast<SoapySDR::Stream *>(stream_);
    }

    void closeStream(SoapySDR::Stream *s) override {
        deactivateStream(s, 0, 0);
        std::lock_guard<std::mutex> lk(ring_mtx_);
        if (reinterpret_cast<Stream *>(s) == stream_) {
            delete stream_;
            stream_ = nullptr;
        }
    }

    size_t getStreamMTU(SoapySDR::Stream *) const override { return chunk_len; }

    int activateStream(SoapySDR::Stream *, const int flags, const long long, const size_t) override {
        if (flags != 0)
            return SOAPY_SDR_NOT_SUPPORTED;
        {
            /* From now, not from whatever the driver held */
            std::lock_guard<std::mutex> rl(radio_mtx_);
            iqradio_flush(radio_);
        }
        std::lock_guard<std::mutex> lk(ring_mtx_);
        head_ = tail_ = 0;
        overflow_ = failed_ = false;
        active_ = true;
        run_cv_.notify_all();
        return 0;
    }

    int deactivateStream(SoapySDR::Stream *, const int, const long long) override {
        std::lock_guard<std::mutex> lk(ring_mtx_);
        active_ = false;
        return 0;
    }

    int readStream(SoapySDR::Stream *s, void *const *buffs, const size_t num, int &flags, long long &time_ns,
            const long timeout_us) override {
        auto *st = reinterpret_cast<Stream *>(s);
        std::unique_lock<std::mutex> lk(ring_mtx_);

        flags = 0;
        time_ns = 0;
        if (!data_cv_.wait_for(lk, std::chrono::microseconds(std::max(0L, timeout_us)),
                    [&] { return head_ != tail_ || overflow_ || failed_ || !active_; }))
            return SOAPY_SDR_TIMEOUT;
        if (overflow_) {
            overflow_ = false;
            return SOAPY_SDR_OVERFLOW;
        }
        if (head_ == tail_)
            return failed_ ? SOAPY_SDR_STREAM_ERROR : SOAPY_SDR_TIMEOUT;

        /* One contiguous stretch of the ring */
        size_t avail = head_ - tail_, at = tail_ % ring_len;
        size_t n = std::min({ num, avail, ring_len - at });
        iqout_convert(st->fmt, &ring_[at], n, buffs[0]);
        tail_ += n;
        return int(n);
    }

    /* Frequency: "RF" on the raster, "BB" the digital shift from there */

    std::vector<std::string> listFrequencies(const int, const size_t) const override { return { "RF", "BB" }; }

    void setFrequency(const int, const size_t, const std::string &name, const double hz,
            const SoapySDR::Kwargs &) override {
        if (name == "RF") {
            retune(hz);
        } else if (name == "BB") {
            double lim = bw_ * 1e6 / 2;
            std::lock_guard<std::mutex> rl(radio_mtx_);
            bb_hz_ = std::clamp(hz, -lim, lim);
            drop_stale();
        } else {
            throw std::runtime_error("uwifisdr: no frequency " + name);
        }
        wait_fresh();
    }

    double getFrequency(const int, const size_t, const std::string &name) const override {
        std::lock_guard<std::mutex> rl(radio_mtx_);
        if (name == "RF")
            return rf_hz_;
        if (name == "BB")
            return bb_hz_;
        throw std::runtime_error("uwifisdr: no frequency " + name);
    }

    SoapySDR::RangeList getFrequencyRange(const int, const size_t, const std::string &name) const override {
        if (name == "BB")
            return { SoapySDR::Range(-(bw_ * 1e6 / 2), bw_ * 1e6 / 2) };
        SoapySDR::RangeList r;
        for (const auto &band : bands())
            if (band.first > 0)
                r.emplace_back(band.first, band.second);
        return r;
    }

    /* Overall: RF plus the shift, half the width past either end of each band */
    SoapySDR::RangeList getFrequencyRange(const int dir, const size_t ch) const override {
        double half = bw_ * 1e6 / 2;
        SoapySDR::RangeList r;
        for (const auto &band : getFrequencyRange(dir, ch, "RF"))
            r.emplace_back(band.minimum() - half, band.maximum() + half);
        return r;
    }

    /* Sample rate and bandwidth: both the channel width */

    std::vector<double> listSampleRates(const int, const size_t) const override {
        std::vector<double> r;
        for (unsigned int w : chip_widths())
            r.push_back(rate_of(info_.chip, w));
        return r;
    }

    SoapySDR::RangeList getSampleRateRange(const int dir, const size_t ch) const override {
        SoapySDR::RangeList r;
        for (double v : listSampleRates(dir, ch))
            r.emplace_back(v, v);
        return r;
    }

    void setSampleRate(const int, const size_t, const double rate) override {
        /* The narrowest width that gives at least this rate */
        auto ws = chip_widths();
        unsigned int w = ws.back();
        for (unsigned int c : ws) {
            if (rate_of(info_.chip, c) >= rate * 0.999) {
                w = c;
                break;
            }
        }
        set_width(w);
    }

    double getSampleRate(const int, const size_t) const override {
        std::lock_guard<std::mutex> rl(radio_mtx_);
        return iqradio_rate(radio_);
    }

    std::vector<double> listBandwidths(const int, const size_t) const override {
        std::vector<double> r;
        for (unsigned int w : chip_widths())
            r.push_back(w * 1e6);
        return r;
    }

    void setBandwidth(const int, const size_t, const double bw) override {
        auto ws = chip_widths();
        unsigned int w = ws.back();
        for (unsigned int c : ws) {
            if (c * 1e6 >= bw * 0.999) {
                w = c;
                break;
            }
        }
        set_width(w);
    }

    double getBandwidth(const int, const size_t) const override { return bw_ * 1e6; }

    /* Gain: one analog stage, held, or the chip's AGC */

    std::vector<std::string> listGains(const int, const size_t) const override { return { "RF" }; }
    bool hasGainMode(const int, const size_t) const override { return true; }

    void setGainMode(const int, const size_t, const bool automatic) override {
        {
            std::lock_guard<std::mutex> rl(radio_mtx_);
            apply_gain(automatic ? 0 : held_db_);
        }
        if (info_.chip == IQDEV_MT7925)
            wait_fresh();
    }

    bool getGainMode(const int, const size_t) const override {
        std::lock_guard<std::mutex> rl(radio_mtx_);
        return iqradio_gain(radio_) == 0;
    }

    void setGain(const int, const size_t, const std::string &, const double db) override {
        {
            std::lock_guard<std::mutex> rl(radio_mtx_);
            /* 0 dB is the lowest held code; the radio takes 0 as the AGC */
            held_db_ = std::max(db, 0.01);
            apply_gain(held_db_);
        }
        if (info_.chip == IQDEV_MT7925)
            wait_fresh();
    }

    void setGain(const int dir, const size_t ch, const double db) override { setGain(dir, ch, "RF", db); }

    double getGain(const int, const size_t, const std::string &) const override {
        std::lock_guard<std::mutex> rl(radio_mtx_);
        return iqradio_gain(radio_);
    }

    double getGain(const int dir, const size_t ch) const override { return getGain(dir, ch, "RF"); }

    SoapySDR::Range getGainRange(const int, const size_t, const std::string &) const override {
        std::lock_guard<std::mutex> rl(radio_mtx_);
        return SoapySDR::Range(0, iqradio_gain_max(radio_, rf_hz_));
    }

    SoapySDR::Range getGainRange(const int dir, const size_t ch) const override {
        return getGainRange(dir, ch, "RF");
    }

private:
    struct Stream {
        iqout_format fmt;
    };

    static void check(int r, const char *what) {
        if (r < 0)
            throw std::runtime_error(std::string("uwifisdr: ") + what + " failed: " + strerror(-r));
    }

    std::vector<unsigned int> chip_widths() const {
        std::vector<unsigned int> r;
        for (unsigned int w : widths)
            if (w <= 80 || info_.chip == IQDEV_MT7925)
                r.push_back(w);
        return r;
    }

    /* The lowest and highest center of each band at the current width; 0 where none */
    std::vector<std::pair<double, double>> bands() const {
        static const std::pair<double, double> spans[] = { { 2412e6, 2484e6 }, { 5005e6, 7115e6 } };
        std::vector<std::pair<double, double>> r;
        for (const auto &s : spans) {
            double lo = 0, hi = 0;
            for (double hz = s.first; hz <= s.second; hz += 5e6) {
                if (!iqradio_center_ok(info_.chip, hz, bw_))
                    continue;
                if (lo == 0)
                    lo = hz;
                hi = hz;
            }
            r.emplace_back(lo, hi);
        }
        return r;
    }

    /* The tunable center nearest hz at the current width */
    double nearest_center(double hz) const {
        double best = 0;
        for (const auto &b : bands()) {
            if (b.first == 0)
                continue;
            double c = std::clamp(b.first + std::round((hz - b.first) / 5e6) * 5e6, b.first, b.second);
            if (best == 0 || std::fabs(c - hz) < std::fabs(best - hz))
                best = c;
        }
        return best;
    }

    /* RF to the raster point nearest hz; the shift stays as the caller sets it */
    void retune(double hz) {
        std::lock_guard<std::mutex> rl(radio_mtx_);
        double c = nearest_center(hz);
        if (c == 0)
            throw std::runtime_error("uwifisdr: nothing tunable at this width");
        check(iqradio_tune(radio_, c, bw_), "tuning");
        rf_hz_ = c;
        double lim = bw_ * 1e6 / 2;
        bb_hz_ = std::clamp(bb_hz_, -lim, lim);
        drop_stale();
    }

    /* Under radio_mtx_: readers get nothing from before the change */
    void drop_stale() {
        tunes_++;
        std::lock_guard<std::mutex> lk(ring_mtx_);
        tail_ = head_;
        overflow_ = false;
    }

    /* Without radio_mtx_: a change returns once the stream after it flows, as a tuner settles */
    void wait_fresh() {
        std::unique_lock<std::mutex> lk(ring_mtx_);
        data_cv_.wait_for(lk, std::chrono::seconds(1),
                [&] { return head_ != tail_ || !active_ || failed_ || quit_; });
    }

    /* A new width keeps the overall frequency, RF moving to a center the width allows */
    void set_width(unsigned int w) {
        double want;
        {
            std::lock_guard<std::mutex> rl(radio_mtx_);
            if (w == bw_)
                return;
            want = rf_hz_ + bb_hz_;
            bw_ = w;
        }
        retune(want);
        {
            std::lock_guard<std::mutex> rl(radio_mtx_);
            double lim = bw_ * 1e6 / 2;
            bb_hz_ = std::clamp(want - rf_hz_, -lim, lim);
            drop_stale();
        }
        wait_fresh();
    }

    /* Under radio_mtx_; the MT7925 takes a new gain at a tune */
    void apply_gain(double db) {
        check(iqradio_set_gain(radio_, db), "setting the gain");
        if (info_.chip == IQDEV_MT7925) {
            check(iqradio_tune(radio_, rf_hz_, bw_), "retuning for the gain");
            drop_stale();
        }
    }

    void read_loop() {
        std::vector<iqradio_sample_t> buf(chunk_len);
        unsigned long losses = 0, last_gen = 0;
        double phase = 0;

        for (;;) {
            {
                std::unique_lock<std::mutex> lk(ring_mtx_);
                run_cv_.wait(lk, [&] { return quit_ || (active_ && !failed_); });
                if (quit_)
                    return;
            }

            double shift, rate;
            int r;
            unsigned long now_losses, gen;
            {
                std::lock_guard<std::mutex> rl(radio_mtx_);
                gen = tunes_;
                r = iqradio_read(radio_, buf.data(), buf.size());
                shift = bb_hz_;
                rate = iqradio_rate(radio_);
                now_losses = iqradio_losses(radio_);
            }

            if (r == 0 && shift != 0) {
                /* Move the wanted center down to 0 Hz */
                std::complex<double> rot = std::polar(1.0, phase), inc = std::polar(1.0, -2 * M_PI * shift / rate);
                for (auto &s : buf) {
                    s *= std::complex<float>(rot);
                    rot *= inc;
                }
                phase = std::arg(rot);
            }

            std::lock_guard<std::mutex> lk(ring_mtx_);
            if (r < 0) {
                SoapySDR::logf(SOAPY_SDR_ERROR, "uwifisdr: %s: the stream stalled or the adapter went away",
                        info_.name);
                failed_ = true;
            } else if (!active_) {
                /* Deactivated during the read */
            } else if (gen != tunes_) {
                /* Read before a retune */
            } else if (ring_len - (head_ - tail_) < buf.size()) {
                overflow_ = true;
            } else {
                for (size_t i = 0; i < buf.size(); i++)
                    ring_[(head_ + i) % ring_len] = buf[i];
                head_ += buf.size();
                /* A tune's restart isn't a loss */
                if (now_losses != losses && gen == last_gen)
                    overflow_ = true;
            }
            last_gen = gen;
            losses = now_losses;
            data_cv_.notify_all();
        }
    }

    iqdev_info info_{};
    std::string mac_;
    iqradio *radio_ = nullptr;

    /* The radio and its tuning */
    mutable std::mutex radio_mtx_;
    /* Changed under radio_mtx_, read by the getters without it */
    std::atomic<unsigned int> bw_{ 20 };
    /* Manual gain until one is set is the band's default */
    double rf_hz_ = 0, bb_hz_ = 0, held_db_ = IQRADIO_GAIN_DEFAULT;

    /* The reader and the ring between it and readStream */
    std::mutex ring_mtx_;
    std::condition_variable run_cv_, data_cv_;
    std::vector<iqradio_sample_t> ring_;
    size_t head_ = 0, tail_ = 0;
    bool active_ = false, overflow_ = false, failed_ = false, quit_ = false;
    /* Bumped under radio_mtx_ at every change of tuning; the reader drops what it read before */
    std::atomic<unsigned long> tunes_{ 0 };
    Stream *stream_ = nullptr;
    std::thread reader_;
};

SoapySDR::KwargsList find_uwifisdr(const SoapySDR::Kwargs &args) {
    SoapySDR::KwargsList r;
    std::string want = arg(args, "device"), serial = arg(args, "serial");

    if (!usb_init(arg(args, "firmware")))
        return r;
    for (const auto &a : list_adapters()) {
        if ((!want.empty() && want != a.name) || (!serial.empty() && serial != a.name))
            continue;
        r.push_back({ { "driver", "uwifisdr" }, { "device", a.name }, { "serial", a.name },
                { "label", std::string(iqdev_chip_name(a.chip)) + " " + a.name }, { "hardware", a.hardware } });
    }
    return r;
}

SoapySDR::Device *make_uwifisdr(const SoapySDR::Kwargs &args) {
    SoapySDR::Kwargs a = args;

    /* find() names adapters by serial too */
    if (a.count("device") == 0 && a.count("serial") != 0)
        a["device"] = a["serial"];
    return new UWifiSDR(a);
}

SoapySDR::Registry registration("uwifisdr", &find_uwifisdr, &make_uwifisdr, SOAPY_SDR_ABI_VERSION);

}
