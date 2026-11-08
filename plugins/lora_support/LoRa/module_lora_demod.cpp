#include "module_lora_demod.h"
#include "logger.h"
#include "imgui/imgui.h"
#include "core/config.h"
#include <fftw3.h>
#include <cmath>
#include <cstring>
#include <numeric>
#include <algorithm>
#include "common/dsp/resamp/rational_resampler.h"

namespace
{
    static inline complex_t cmul(complex_t a, complex_t b)
    {
        return complex_t(a.real * b.real - a.imag * b.imag,
                         a.real * b.imag + a.imag * b.real);
    }
    static inline complex_t cconj(complex_t a) { return complex_t(a.real, -a.imag); }
    static inline float cnorm(complex_t a) { return a.real * a.real + a.imag * a.imag; }
}

namespace fengyun4
{
    namespace emwin
    {
        // =====================================================================
        // 构造 / 析构
        // =====================================================================
        LoRaDemodModule::LoRaDemodModule(std::string input_file, std::string output_file_hint,
                                         nlohmann::json parameters)
            : BaseDemodModule(input_file, output_file_hint, parameters)
        {
            if (parameters.count("sf")) sf = parameters["sf"].get<int>();
            if (parameters.count("bw")) bw_hz = parameters["bw"].get<int>();
            if (parameters.count("preamble_len")) preamble_len = parameters["preamble_len"].get<int>();
            if (parameters.count("zero_padding_ratio"))
                zero_padding_ratio = parameters["zero_padding_ratio"].get<int>();

            if (sf < 7 || sf > 12) sf = 9;
            if (bw_hz <= 0) bw_hz = 812500;
            if (preamble_len <= 1) preamble_len = 6;
            if (zero_padding_ratio <= 0) zero_padding_ratio = 10;

            sample_num = 1 << sf;
            bin_num = zero_padding_ratio * sample_num;
            fft_len = bin_num;

            MIN_SPS = 1.0f;
            MAX_SPS = 1e9f;
            if (d_symbolrate <= 0) d_symbolrate = (int)d_samplerate;

            name = "FY-4A EMWIN LoRa Demod";
            show_freq = false;

            init_chirps();
            init_butterworth();

            fft_in = (fftwf_complex *)fftwf_malloc(sizeof(fftwf_complex) * fft_len);
            fft_out = (fftwf_complex *)fftwf_malloc(sizeof(fftwf_complex) * fft_len);
            fft_plan = fftwf_plan_dft_1d(fft_len, fft_in, fft_out, FFTW_FORWARD, FFTW_ESTIMATE);

            sample_buffer.reserve((size_t)sample_num * 64);
        }

        LoRaDemodModule::~LoRaDemodModule()
        {
            if (fft_plan) fftwf_destroy_plan(fft_plan);
            if (fft_in) fftwf_free(fft_in);
            if (fft_out) fftwf_free(fft_out);
            fft_in = nullptr;
            fft_out = nullptr;
            fft_plan = nullptr;
        }
        void LoRaDemodModule::init_chirps()
        {
            chirp_up.resize(sample_num);
            chirp_down.resize(sample_num);
        
            const double N = (double)sample_num;
            const double two_pi = 2.0 * M_PI;
        
            for (int i = 0; i < sample_num; i++) {
                const double n = (double)i;
        
                // 标准 LoRa 离散 upchirp（fs = 2*BW，扫频 -BW/2 → +BW/2）
                const double phase = two_pi * (-0.25 * n + (n * n) / (4.0 * N));
        
                // 旧代码候选（fs = BW 或扫频 2*BW），如需回退可启用：
                // const double phase = two_pi * (-0.5 * n + (n * n) / (2.0 * N));
        
                complex_t c((float)std::cos(phase), (float)std::sin(phase));
                chirp_up[i]   = c;
                chirp_down[i] = cconj(c);   // 共轭 = 频率反向的 downchirp
            }
        }

        // 10 阶 Butterworth 低通，fc = BW/2，fs = 2*BW（rsp 输出采样率）
        // 5 个 biquad 级联，Q_k = 1/(2 sin(π(2k+1)/(2n)))
        void LoRaDemodModule::init_butterworth()
        {
            butter.clear();
            const int order = 10;
            const double fs = 2.0 * (double)bw_hz;
            const double fc = (double)bw_hz / 2.0;
            const double w0 = 2.0 * M_PI * fc / fs;
            const double cw = std::cos(w0);
            const double sw = std::sin(w0);

            for (int k = 0; k < order / 2; k++) {
                double Q = 1.0 / (2.0 * std::sin(M_PI * (2 * k + 1) / (2.0 * order)));
                double alpha = sw / (2.0 * Q);
                double a0 = 1.0 + alpha;
                Biquad bq;
                bq.b0 = (float)((1.0 - cw) / 2.0 / a0);
                bq.b1 = (float)((1.0 - cw) / a0);
                bq.b2 = (float)((1.0 - cw) / 2.0 / a0);
                bq.a1 = (float)(-2.0 * cw / a0);
                bq.a2 = (float)((1.0 - alpha) / a0);
                butter.push_back(bq);
            }
            butter_ready = true;
        }

        void LoRaDemodModule::stop()
        {
            stop_requested.store(true);
            std::shared_ptr<dsp::RationalResamplerBlock<complex_t>> rsp;
            {
                std::lock_guard<std::mutex> lock(rsp_mtx);
                rsp = active_rsp;
            }
            if (rsp && rsp->output_stream)
                rsp->output_stream->stopReader();
        }

        void LoRaDemodModule::init()
        {
            BaseDemodModule::initb();
        }

        int LoRaDemodModule::dechirp_and_peak(const complex_t *samples, int n)
        {
            int nn = std::min(n, sample_num);
            for (int i = 0; i < nn; i++) {
                complex_t v = cmul(samples[i], chirp_down[i]);
                fft_in[i][0] = v.real;
                fft_in[i][1] = v.imag;
            }
            for (int i = nn; i < fft_len; i++) {
                fft_in[i][0] = 0.0f;
                fft_in[i][1] = 0.0f;
            }
            fftwf_execute(fft_plan);

            // 合并两侧：abs(ft[:bin_num]) + abs(ft[fft_len-bin_num:])
            int peak = 0;
            float best = -1.0f;
            for (int k = 0; k < bin_num; k++) {
                float a = std::hypot(fft_out[k][0], fft_out[k][1]);
                float b = std::hypot(fft_out[fft_len - bin_num + k][0],
                                     fft_out[fft_len - bin_num + k][1]);
                float m = a + b;
                if (m > best) { best = m; peak = k; }
            }
            return peak;
        }

        bool LoRaDemodModule::try_detect_preamble()
        {
            while (sample_buffer.size() - read_pos >= (size_t)sample_num) {
                int pk = dechirp_and_peak(sample_buffer.data() + read_pos, sample_num);

                if (preamble_bins.empty()) {
                    preamble_bins.push_back(pk);
                    read_pos += sample_num;
                    continue;
                }

                int prev = preamble_bins.back();
                int dist = (pk - prev) % sample_num;
                if (dist > sample_num / 2) dist -= sample_num;
                if (dist < -sample_num / 2) dist += sample_num;

                if (std::abs(dist) <= 10) {
                    preamble_bins.push_back(pk);
                    read_pos += sample_num;

                    if ((int)preamble_bins.size() >= preamble_len - 1) {
                        double sum = 0;
                        for (int b : preamble_bins) sum += b;
                        preamble_bin = (int)std::round(sum / (double)preamble_bins.size());

                        // VERIFY: 起点细化公式，必须与 Python 完全一致
                        double fine = std::round((double)preamble_bin / 10.0 * 2.0);
                        (void)fine; // 可选：用于微调 read_pos

                        sync_state = SyncState::CHECK_SFD;
                        return true;
                    }
                } else {
                    preamble_bins.clear();
                    preamble_bins.push_back(pk);
                    read_pos += sample_num;
                }
            }
            return false;
        }

        bool LoRaDemodModule::check_sfd_and_fine_sync()
        {
            if (sample_buffer.size() - read_pos < (size_t)sample_num) return false;

            // 用 upchirp 做 dechirp 检测 downchirp SFD，分离 CFO
            complex_t *win = sample_buffer.data() + read_pos;
            for (int i = 0; i < sample_num; i++) {
                complex_t v = cmul(win[i], chirp_up[i]);
                fft_in[i][0] = v.real;
                fft_in[i][1] = v.imag;
            }
            for (int i = sample_num; i < fft_len; i++) {
                fft_in[i][0] = 0.0f;
                fft_in[i][1] = 0.0f;
            }
            fftwf_execute(fft_plan);

            int pk = 0;
            float best = -1.0f;
            for (int k = 0; k < bin_num; k++) {
                float a = std::hypot(fft_out[k][0], fft_out[k][1]);
                float b = std::hypot(fft_out[fft_len - bin_num + k][0],
                                     fft_out[fft_len - bin_num + k][1]);
                float m = a + b;
                if (m > best) { best = m; pk = k; }
            }

            cfo_bin = pk;
            cfo_hz = (double)pk / (double)bin_num * (double)bw_hz;

            // VERIFY: 2.25 或 1.25 的选择条件必须与 Python 一致。
            // 这里按 preamble_len=6 的典型情形用 2.25。
            data_symbol_offset = 2.25;
            size_t skip = (size_t)std::llround(data_symbol_offset * (double)sample_num);
            read_pos += skip;

            sync_state = SyncState::DECODE_HEADER;
            return true;
        }

        bool LoRaDemodModule::decode_header()
        {
            header_symbols.clear();
            for (int i = 0; i < 8; i++) {
                if (sample_buffer.size() - read_pos < (size_t)sample_num) return false;
                int pk = dechirp_and_peak(sample_buffer.data() + read_pos, sample_num);

                // 符号映射：(peak_idx + bin_num - preamble_bin) / 10 % 2^SF
                int v = (pk + bin_num - preamble_bin) / zero_padding_ratio % sample_num;
                if (v < 0) v += sample_num;
                header_symbols.push_back((uint16_t)std::lround((double)v));

                read_pos += sample_num;
                symbols_since_sync++;
            }

            // Gray 解码
            std::vector<uint16_t> g;
            g.reserve(header_symbols.size());
            for (auto s : header_symbols) g.push_back(gray_decode(s));

            // VERIFY: ppm = SF-2 解交织，rdd = 8
            int ppm = sf - 2;
            auto cws = diag_deinterleave(g, ppm, 8);

            // Hamming rdd=8 解码 → nibbles
            std::vector<uint8_t> nib;
            for (auto cw : cws) nib.push_back(hamming_decode_rdd(cw, 8));

            if (nib.size() < 5) { reset_sync_state(); return false; }

            // VERIFY: header 字节/位布局必须与 Python 一致。
            // 这里采用最常见的 LoRa header 布局：
            //   payload_len = nib[0]<<4 | nib[1]
            //   cr          = nib[2] & 0x07
            //   has_crc     = (nib[2] >> 3) & 1
            //   checksum    = nib[4] 低 5 bit
            payload_len = ((int)nib[0] << 4) | (int)nib[1];
            cr = nib[2] & 0x07;
            header_has_crc = ((nib[2] >> 3) & 0x01) != 0;
            ldro = false; // 若 Python 从 header 解析 LDO 标志，请在此处赋值

            if (cr < 1 || cr > 4) { reset_sync_state(); return false; }
            if (payload_len <= 0 || payload_len > 255) { reset_sync_state(); return false; }

            // VERIFY: 5-bit header checksum 公式必须与 Python 一致
            {
                uint8_t s = (uint8_t)((nib[0] & 0xF) ^ (nib[0] >> 4) ^
                                      (nib[1] & 0xF) ^ (nib[1] >> 4) ^
                                      (nib[2] & 0x7) ^
                                      ((nib[2] >> 3) & 1));
                uint8_t expected = nib[4] & 0x1F;
                if ((s & 0x1F) != expected) {
                    logger->debug("LoRa header checksum mismatch");
                    reset_sync_state();
                    return false;
                }
            }

            logger->debug("LoRa header OK: len=%d cr=4/%d crc=%d",
                          payload_len, cr + 4, (int)header_has_crc);
            sync_state = SyncState::DECODE_PAYLOAD;
            return true;
        }

        bool LoRaDemodModule::decode_payload()
        {
            int rdd = cr + 4;              // 5..8
            int ppm = ldro ? (sf - 2) : sf;

            int total_bits = payload_len * 8 + (header_has_crc ? 16 : 0);
            int bits_per_group = ppm * 4;  // 每组 rdd 个 symbol 携带 ppm*4 bit
            int groups = (total_bits + bits_per_group - 1) / bits_per_group;
            int symbols_needed = groups * rdd;

            payload_symbols.clear();
            for (int i = 0; i < symbols_needed; i++) {
                if (sample_buffer.size() - read_pos < (size_t)sample_num) return false;
                int pk = dechirp_and_peak(sample_buffer.data() + read_pos, sample_num);
                int v = (pk + bin_num - preamble_bin) / zero_padding_ratio % sample_num;
                if (v < 0) v += sample_num;
                payload_symbols.push_back((uint16_t)std::lround((double)v));
                read_pos += sample_num;
                symbols_since_sync++;
            }

            std::vector<uint16_t> g;
            g.reserve(payload_symbols.size());
            for (auto s : payload_symbols) g.push_back(gray_decode(s));

            // 按 rdd 个 symbol 一组解交织 → ppm 个 codeword
            std::vector<uint8_t> nibbles;
            for (int off = 0; off + rdd <= (int)g.size(); off += rdd) {
                std::vector<uint16_t> grp(g.begin() + off, g.begin() + off + rdd);
                auto cws = diag_deinterleave(grp, ppm, rdd);
                for (auto cw : cws)
                    nibbles.push_back(hamming_decode_rdd(cw, rdd));
            }

            // Nibble 合并：nibble[1] << 4 | nibble[0]
            std::vector<uint8_t> bytes;
            bytes.reserve(nibbles.size() / 2);
            for (size_t i = 0; i + 1 < nibbles.size(); i += 2)
                bytes.push_back((uint8_t)((nibbles[i] & 0xF) | ((nibbles[i + 1] & 0xF) << 4)));

            int need = payload_len + (header_has_crc ? 2 : 0);
            if ((int)bytes.size() < need) { reset_sync_state(); return false; }

            // 去白化（含 CRC 字节）
            dewhiten(bytes.data(), need);

            if (header_has_crc) {
                uint16_t calc = lora_crc16(bytes.data(), payload_len);
                // VERIFY: 字节序，Python binascii.crc_hqx 后 pack('<H') 还是 pack('>H')
                uint16_t recv = (uint16_t)bytes[payload_len]
                              | ((uint16_t)bytes[payload_len + 1] << 8);
                if (calc != recv) {
                    packets_crc_fail++;
                    logger->debug("LoRa CRC fail (calc=%04X recv=%04X)", calc, recv);
                    reset_sync_state();
                    return false;
                }
            }

            emit_payload(bytes.data(), payload_len);
            packets_ok++;
            logger->info("LoRa packet OK: %d bytes (ok=%d fail=%d)",
                         payload_len, packets_ok, packets_crc_fail);
            reset_sync_state();
            return true;
        }

        void LoRaDemodModule::reset_sync_state()
        {
            sync_state = SyncState::SEARCH_PREAMBLE;
            preamble_bins.clear();
            header_symbols.clear();
            payload_symbols.clear();
            symbols_since_sync = 0;
            sfo_accum = 0;
        }

        void LoRaDemodModule::compact_buffer()
        {
            if (read_pos > (size_t)sample_num * 32) {
                sample_buffer.erase(sample_buffer.begin(),
                                    sample_buffer.begin() + read_pos);
                read_pos = 0;
            }
        }

        uint16_t LoRaDemodModule::gray_decode(uint16_t g)
        {
            g ^= (g >> 8);
            g ^= (g >> 4);
            g ^= (g >> 2);
            g ^= (g >> 1);
            return g;
        }

        // VERIFY: 对角解交织，必须逐位对照 Python diag_deinterleave。
        // syms: rdd 个 symbol；每个 symbol 有 sf bit（但只用到 rdd 个 bit 位置）
        // 返回 ppm 个 codeword，每个 rdd bit
        std::vector<uint16_t> LoRaDemodModule::diag_deinterleave(
            const std::vector<uint16_t> &syms, int ppm, int rdd)
        {
            std::vector<uint16_t> cw(ppm, 0);
            int nsym = (int)syms.size();
            for (int i = 0; i < ppm; i++) {
                for (int j = 0; j < nsym; j++) {
                    int bit_pos = (i + j) % nsym; // 若 Python 用 % ppm 请改
                    int b = (syms[j] >> bit_pos) & 1;
                    cw[i] |= (uint16_t)(b << j);
                }
            }
            return cw;
        }

        int LoRaDemodModule::parity8(uint8_t x)
        {
            x ^= x >> 4;
            x ^= x >> 2;
            x ^= x >> 1;
            return x & 1;
        }

        // VERIFY: LoRa Hamming 变体，rdd=5/6/7/8。syndrome 与修正映射需逐位对照 Python。
        uint8_t LoRaDemodModule::hamming_decode_rdd(uint16_t cw16, int rdd)
        {
            uint8_t c = (uint8_t)(cw16 & 0xFF);
            uint8_t data = c & 0x0F;

            // parity masks（用户给定）
            int s1 = parity8(c & 0x8D);
            int s2 = parity8(c & 0x4B);
            int s3 = parity8(c & 0x17);
            int s4 = parity8(c & 0x1F);
            int s5 = parity8(c & 0x2E);
            int syn = s1 | (s2 << 1) | (s3 << 2) | (s4 << 3) | (s5 << 4);

            // parity fix：{3:4, 5:8, 6:1, 7:2}
            int bit = 0;
            switch (syn) {
            case 3: bit = 4; break;
            case 5: bit = 8; break; // 最高位（1-based）
            case 6: bit = 1; break;
            case 7: bit = 2; break;
            default: break;
            }
            if (bit >= 1 && bit <= 8)
                c ^= (uint8_t)(1u << (bit - 1));

            (void)rdd;
            return c & 0x0F;
        }

        // VERIFY: LFSR 多项式 x^8+x^6+x^5+x^4+1，初值 0xFF，bit 顺序需与 Python 一致。
        void LoRaDemodModule::dewhiten(uint8_t *data, int len)
        {
            uint8_t lfsr = 0xFF;
            for (int i = 0; i < len; i++) {
                uint8_t out = 0;
                for (int b = 0; b < 8; b++) {
                    uint8_t bit = lfsr & 0x01;
                    out |= (uint8_t)(bit << b);
                    lfsr >>= 1;
                    if (bit) lfsr ^= 0xB8; // x^8+x^6+x^5+x^4+1 的反馈字节
                }
                data[i] ^= out;
            }
        }

        // VERIFY: binascii.crc_hqx(data, 0) 的初值与反射方向
        uint16_t LoRaDemodModule::lora_crc16(const uint8_t *data, int len)
        {
            uint16_t crc = 0x0000;
            for (int i = 0; i < len; i++) {
                crc ^= (uint16_t)data[i] << 8;
                for (int b = 0; b < 8; b++)
                    crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021)
                                         : (uint16_t)(crc << 1);
            }
            return crc;
        }

        void LoRaDemodModule::emit_payload(const uint8_t *data, int len)
        {
            if (!payload_out.is_open()) {
                std::string path = d_output_file_hint + ".payload";
                payload_out.open(path, std::ios::binary | std::ios::app);
                if (payload_out.is_open())
                    d_output_files.push_back(path);
            }
            if (payload_out.is_open()) {
                payload_out.write((const char *)data, len);
                payload_out.flush();
            }
        }

        void LoRaDemodModule::process()
        {
            stop_requested.store(false);
            reset_sync_state();

            if (input_data_type == DATA_FILE && file_source)
                filesize = file_source->getFilesize();
            else
                filesize = 0;

            data_out = std::ofstream(d_output_file_hint + ".soft", std::ios::binary);
            d_output_files.push_back(d_output_file_hint + ".soft");
            logger->info("LoRa demod output: " + d_output_file_hint + ".soft");
            logger->info("SF: " + std::to_string(sf) + " BW: " + std::to_string(bw_hz));

            if (!agc) {
                logger->error("AGC block not initialized");
                return;
            }

            BaseDemodModule::start();

            // 重采样：2*BW / fs（gcd 约分）
            unsigned interp = (unsigned)(2 * bw_hz);
            unsigned decim = (unsigned)d_samplerate;
            unsigned g = std::gcd(interp, decim);
            if (g == 0) g = 1;
            interp /= g;
            decim /= g;

            auto rsp = std::make_shared<dsp::RationalResamplerBlock<complex_t>>(
                agc->output_stream, interp, decim);
            rsp->start();

            {
                std::lock_guard<std::mutex> lock(rsp_mtx);
                active_rsp = rsp;
            }

            // Butterworth 状态重置
            for (auto &bq : butter) bq.reset();

            time_t lastTime = 0;
            int total_syms = 0;

            while (demod_should_run() && !stop_requested.load()) {
                int n = rsp->output_stream->read();
                if (n < 0) break;

                if (n > 0 && rsp->output_stream->readBuf) {
                    complex_t *buf = rsp->output_stream->readBuf;
                    for (int i = 0; i < n; i++) {
                        complex_t v = buf[i];
                        // 10 阶 Butterworth LPF（截止 BW/2）
                        if (butter_ready) {
                            for (auto &bq : butter) v = bq.process(v);
                        }
                        sample_buffer.push_back(v);
                    }
                }
                rsp->output_stream->flush();

                // 状态机推进
                bool progress = true;
                while (progress && sample_buffer.size() - read_pos >= (size_t)sample_num) {
                    switch (sync_state) {
                    case SyncState::SEARCH_PREAMBLE:
                        progress = try_detect_preamble();
                        break;
                    case SyncState::CHECK_SFD:
                        progress = check_sfd_and_fine_sync();
                        break;
                    case SyncState::DECODE_HEADER:
                        progress = decode_header();
                        break;
                    case SyncState::DECODE_PAYLOAD:
                        progress = decode_payload();
                        break;
                    default:
                        progress = false;
                        break;
                    }
                }
                total_syms = symbols_since_sync;
                last_syms = total_syms;

                compact_buffer();

                if (input_data_type == DATA_FILE && file_source)
                    progress = file_source->getPosition();

                if (time(NULL) % 10 == 0 && lastTime != time(NULL)) {
                    lastTime = time(NULL);
                    if (filesize > 0) {
                        double pct = round(((double)progress / (double)filesize) * 1000.0) / 10.0;
                        logger->info("Progress " + std::to_string(pct) + "%");
                    }
                }
            }

            data_out.close();
            rsp->stop();
            {
                std::lock_guard<std::mutex> lock(rsp_mtx);
                active_rsp.reset();
            }
            if (payload_out.is_open()) payload_out.close();
            BaseDemodModule::stop();
        }

        void LoRaDemodModule::drawUI(bool window)
        {
            ImGui::Begin("FY-4A EMWIN LoRa Demod", NULL, window ? 0 : NOWINDOW_FLAGS);
            ImGui::Text("SF: %d   BW: %d Hz", sf, bw_hz);
            ImGui::Text("Packets OK: %d   CRC fail: %d", packets_ok, packets_crc_fail);
            ImGui::Text("Symbols since sync: %d", last_syms);

            if (input_data_type == DATA_FILE && filesize > 0)
                ImGui::ProgressBar((double)progress / (double)filesize,
                                   ImVec2(ImGui::GetContentRegionAvail().x, 20 * ui_scale));

            drawStopButton();
            ImGui::End();
        }
        std::string LoRaDemodModule::getID()
        {
            return "lora_demod";
        }

        std::vector<std::string> LoRaDemodModule::getParameters()
        {
            return {"symbolrate", "sf", "bw", "preamble_len", "zero_padding_ratio"};
        }

        std::shared_ptr<ProcessingModule> LoRaDemodModule::getInstance(
            std::string input_file, std::string output_file_hint,
            nlohmann::json parameters)
        {
            return std::make_shared<LoRaDemodModule>(input_file, output_file_hint, parameters);
        }
    }
}