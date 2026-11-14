#include "module_lora_demod.h"
#include "logger.h"
#include "imgui/imgui.h"
#include "core/config.h"
#include <cmath>
#include <cstring>
#include <numeric>
#include <algorithm>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace fengyun4
{
    namespace emwin
    {
        // ================= 核心算法辅助函数 =================
        static uint8_t xorbits16(uint16_t data, uint16_t mask) {
            uint16_t v = data & mask;
            uint8_t r = 0;
            for (int i = 0; i < 16; ++i) r ^= (v >> i) & 1;
            return r;
        }

        static uint16_t crc_hqx(const uint8_t* data, size_t len) {
            uint16_t crc = 0;
            for (size_t i = 0; i < len; ++i) {
                crc ^= (uint16_t)data[i] << 8;
                for (int j = 0; j < 8; ++j) {
                    if (crc & 0x8000) crc = (uint16_t)((crc << 1) ^ 0x1021);
                    else crc <<= 1;
                }
            }
            return crc;
        }

        static std::pair<uint8_t, uint8_t> calc_crc(const std::vector<uint8_t>& data) {
            if (data.empty()) return {0, 0};
            if (data.size() == 1) return {data[0], 0};
            if (data.size() == 2) return {data[1], data[0]};
            uint16_t seq = crc_hqx(data.data(), data.size() - 2);
            uint8_t high = (uint8_t)((seq >> 8) ^ data[data.size() - 2]);
            uint8_t low = (uint8_t)((seq & 0xff) ^ data[data.size() - 1]);
            return {low, high};
        }

        static std::vector<uint8_t> calc_checksum(const std::vector<uint8_t>& data) {
            static const uint8_t M[5][12] = {
                {1,1,1,1,0,0,0,0,0,0,0,0},
                {1,0,0,0,1,1,1,0,0,0,0,1},
                {0,1,0,0,1,0,0,1,1,0,1,0},
                {0,0,1,0,0,1,0,1,0,1,1,1},
                {0,0,0,1,0,0,1,0,1,1,1,1}
            };
            std::vector<uint8_t> result(5, 0);
            for (int i = 0; i < 5; ++i) {
                int sum = 0;
                for (int j = 0; j < 12; ++j) sum += M[i][j] * data[j];
                result[i] = (uint8_t)(sum & 1);
            }
            return result;
        }

        static void dewhiten(std::vector<uint8_t>& data) {
            uint8_t reg = 0xff;
            for (size_t i = 0; i < data.size(); ++i) {
                data[i] ^= reg;
                reg = (uint8_t)(((reg << 1) & 0xff) ^ ((reg >> 7) & 1) ^ ((reg >> 5) & 1) ^
                                ((reg >> 4) & 1) ^ ((reg >> 3) & 1));
            }
        }
        // ====================================================

        LoRaDemodModule::LoRaDemodModule(std::string input_file, std::string output_file_hint, nlohmann::json parameters)
            : BaseDemodModule(input_file, output_file_hint, parameters)
        {
            if (parameters.count("sf")) sf = parameters["sf"].get<int>();
            if (parameters.count("bw")) bw_hz = parameters["bw"].get<int>();
            if (parameters.count("preamble_len")) preamble_len = parameters["preamble_len"].get<int>();
            if (parameters.count("zero_padding_ratio")) zero_padding_ratio = parameters["zero_padding_ratio"].get<int>();

            if (sf < 7 || sf > 12) sf = 9;
            if (bw_hz <= 0) bw_hz = 812500;
            if (preamble_len <= 1) preamble_len = 6;
            if (zero_padding_ratio <= 0) zero_padding_ratio = 10;

            MIN_SPS = 1.0f;
            MAX_SPS = 1e9f;
            if (d_symbolrate <= 0) d_symbolrate = (int)bw_hz;

            name = "Generic LoRa Demod";
            show_freq = true;
            dc_state = complex_t(0, 0);
        }

        LoRaDemodModule::~LoRaDemodModule()
        {
            if (fft_plan) fftwf_destroy_plan(fft_plan);
            if (fft_in) fftwf_free(fft_in);
            if (fft_out) fftwf_free(fft_out);
        }

        void LoRaDemodModule::init()
        {
            BaseDemodModule::initb();
            
            // 信任上层传入的采样率，如果是 0 则安全兜底
            if (d_samplerate <= 0) d_samplerate = 2048000; 

            sample_num = (int)std::round(std::pow(2.0, sf) * (double)d_samplerate / (double)bw_hz);
            if (sample_num <= 0) sample_num = 1291;
            
            bin_num = zero_padding_ratio * (1 << sf);
            fft_len = sample_num * zero_padding_ratio;
            
            if (fft_len <= 0) return;

            logger->info("LoRa Demod Init: FS=%d, BW=%d, SF=%d", d_samplerate, bw_hz, sf);
            logger->info("Calculated sample_num=%d, bin_num=%d, fft_len=%d", sample_num, bin_num, fft_len);

            init_chirps((double)d_samplerate);

            fft_in = (fftwf_complex *)fftwf_malloc(sizeof(fftwf_complex) * fft_len);
            fft_out = (fftwf_complex *)fftwf_malloc(sizeof(fftwf_complex) * fft_len);
            
            if (!fft_in || !fft_out) {
                logger->error("Failed to allocate FFTW arrays for LoRa Demod");
                return;
            }
            
            // 初始全清空一次，后续不再重复清空
            memset(fft_in, 0, sizeof(fftwf_complex) * fft_len);
            
            fft_plan = fftwf_plan_dft_1d(fft_len, fft_in, fft_out, FFTW_FORWARD, FFTW_ESTIMATE);
            if (!fft_plan) {
                logger->error("Failed to create FFTW plan for LoRa Demod");
                return;
            }

            sample_buffer.reserve((size_t)sample_num * 128);
        }

        void LoRaDemodModule::init_chirps(double fs)
        {
            chirp_up.resize(sample_num);
            chirp_down.resize(sample_num);
            double bw = (double)bw_hz;
            int num_symbols = 1 << sf;
            double seconds_per_symbol = num_symbols / bw;
            
            double dfdt = bw / seconds_per_symbol;
            double f0 = -bw / 2.0;

            for (int i = 0; i < sample_num; i++) {
                double t = (double)i / fs;
                double f = f0 + 0.5 * dfdt * t;
                double phase = 2.0 * M_PI * f * t;
                complex_t c((float)std::cos(phase), (float)std::sin(phase));
                chirp_up[i] = c;
                chirp_down[i] = complex_t(c.real, -c.imag);
            }
        }

        void LoRaDemodModule::stop()
        {
            stop_requested.store(true);
            if (input_data_type == DATA_FILE && file_source)
                file_source->output_stream->stopReader();
        }

        std::pair<float, int> LoRaDemodModule::dechirp(const std::vector<complex_t>& buf, size_t pos, bool is_up)
        {
            if (pos >= buf.size() || !fft_in || !fft_plan) return {0.0f, 0};
            const auto& chp = is_up ? chirp_down : chirp_up;
            size_t signal_len = std::min((size_t)sample_num, buf.size() - pos);
            
            // 速度优化：不再 memset，因为初始已经清零，且后续只覆盖前 sample_num 个。
            for (size_t i = 0; i < signal_len; ++i) {
                complex_t a = buf[pos + i];
                complex_t b = chp[i];
                complex_t v(a.real * b.real - a.imag * b.imag, a.real * b.imag + a.imag * b.real);
                fft_in[i][0] = v.real;
                fft_in[i][1] = v.imag;
            }
            
            fftwf_execute(fft_plan);

            float best_mag = 0.0f;
            int best = 0;
            // 速度优化：去除 std::hypot，直接比较平方和
            for (int i = 0; i < bin_num; ++i) {
                float m1sq = fft_out[i][0]*fft_out[i][0] + fft_out[i][1]*fft_out[i][1];
                float m2sq = fft_out[fft_len - bin_num + i][0]*fft_out[fft_len - bin_num + i][0] + 
                             fft_out[fft_len - bin_num + i][1]*fft_out[fft_len - bin_num + i][1];
                float msq = m1sq + m2sq;
                if (msq > best_mag) { best_mag = msq; best = i; }
            }
            return {std::sqrt(best_mag), best}; // 仅在最后计算一次 sqrt
        }

        int LoRaDemodModule::peak_idx_to_symbol(int peak_idx, int pre_bin)
        {
            int normalized = (peak_idx + bin_num - pre_bin) % bin_num;
            if (normalized < 0) normalized += bin_num;
            double sym = (double)normalized / zero_padding_ratio;
            return (int)std::round(std::fmod(sym, (double)(1 << sf)));
        }

        size_t LoRaDemodModule::detect(const std::vector<complex_t>& buf, size_t start)
        {
            if (buf.size() < (size_t)sample_num * preamble_len + 1) throw std::runtime_error("no preamble");

            std::vector<int> pk;
            while (start < buf.size() - (size_t)sample_num * preamble_len - 1) {
                if ((int)pk.size() == preamble_len - 1) {
                    long long back = (long long)std::round((double)pk.back() / zero_padding_ratio * 2);
                    long long p = (long long)start - back;
                    if (p < 0) p = 0;
                    return (size_t)p;
                }
                auto r = dechirp(buf, start, true);
                int pk0 = r.second;
                if (!pk.empty()) {
                    int diff = (pk.back() - pk0) % bin_num;
                    if (diff < 0) diff += bin_num;
                    if (diff > bin_num / 2) diff = bin_num - diff;
                    if (diff <= zero_padding_ratio * 10) pk.push_back(pk0);
                    else pk = {pk0};
                } else {
                    pk = {pk0};
                }
                start += sample_num;
            }
            throw std::runtime_error("no preamble");
        }

        LoRaDemodModule::SyncResult LoRaDemodModule::sync(const std::vector<complex_t>& buf, size_t start)
        {
            if (buf.size() < (size_t)sample_num) throw std::runtime_error("no sync");

            bool found = false;
            while (start < buf.size() - (size_t)sample_num) {
                auto pu = dechirp(buf, start, true);
                auto pd = dechirp(buf, start, false);
                start += sample_num;
                if (pd.first > pu.first) { found = true; break; }
            }
            if (!found) throw std::runtime_error("no sync");

            auto r = dechirp(buf, start, false);
            int pkd_bin = r.second;
            long long off;
            if (pkd_bin + 1 > bin_num / 2) off = (long long)std::round((double)(pkd_bin - bin_num) / zero_padding_ratio);
            else off = (long long)std::round((double)pkd_bin / zero_padding_ratio);
            long long st = (long long)start + off;
            if (st < 0) st = 0;
            start = (size_t)st;

            size_t p4 = start >= (size_t)(4 * sample_num) ? start - 4 * sample_num : 0;
            auto rp = dechirp(buf, p4, true);
            int pre_bin = rp.second;
            double cfo;
            if (pre_bin + 1 > bin_num / 2) cfo = (pre_bin - bin_num) * bw_hz / bin_num;
            else cfo = pre_bin * bw_hz / bin_num;

            size_t p1 = start >= (size_t)sample_num ? start - sample_num : 0;
            auto pku = dechirp(buf, p1, true);
            auto pkd = dechirp(buf, p1, false);
            size_t new_start;
            if (pku.first > pkd.first) new_start = start + (size_t)std::round(2.25 * sample_num);
            else new_start = start + (size_t)std::round(1.25 * sample_num);
            
            return {new_start, pre_bin, cfo};
        }

        std::vector<double> LoRaDemodModule::dynamic_compensation(const std::vector<uint16_t>& data, double cfo)
        {
            int n = (int)data.size();
            int mod = 1 << sf;
            std::vector<double> symbols(n);
            double fc = 1.697e9; 

            for (int i = 0; i < n; ++i) {
                double drift = (i + 2) * (double)mod * cfo / fc;
                double v = std::fmod((double)data[i] - drift, (double)mod);
                if (v < 0) v += mod;
                symbols[i] = v;
            }
            if (ldro) {
                double bin_offset = 0.0, v_last = 1.0, modulus = 4.0;
                for (int i = 0; i < n; ++i) {
                    double v = symbols[i];
                    double d = std::fmod(v - v_last, modulus);
                    if (d < 0) d += modulus;
                    if (d < modulus / 2) bin_offset -= d;
                    else bin_offset -= (d - modulus);
                    v_last = v;
                    double s = std::fmod(v + bin_offset, (double)mod);
                    if (s < 0) s += mod;
                    symbols[i] = s;
                }
            }
            return symbols;
        }

        std::vector<uint16_t> LoRaDemodModule::gray_coding(const std::vector<double>& data)
        {
            int n = (int)data.size();
            int mod = 1 << sf;
            std::vector<uint16_t> r(n);
            int head = std::min(8, n);
            for (int i = 0; i < head; ++i) r[i] = (uint16_t)std::floor(data[i] / 4.0);
            if (ldro) {
                for (int i = 8; i < n; ++i) r[i] = (uint16_t)std::floor(data[i] / 4.0);
            } else {
                for (int i = 8; i < n; ++i) {
                    double v = std::fmod(data[i] - 1.0, (double)mod);
                    if (v < 0) v += mod;
                    r[i] = (uint16_t)std::round(v);
                }
            }
            for (int i = 0; i < n; ++i) r[i] = (uint16_t)((r[i] >> 1) ^ r[i]);
            return r;
        }

        std::vector<uint16_t> LoRaDemodModule::diag_deinterleave(const std::vector<uint16_t>& symbols, int ppm)
        {
            int n = (int)symbols.size();
            std::vector<uint16_t> result(ppm, 0);
            uint16_t mask = (uint16_t)((1 << ppm) - 1);
            for (int i = 0; i < n; ++i) {
                int shift = i % ppm;
                uint16_t val = symbols[i] & mask;
                uint16_t shifted;
                if (shift == 0) shifted = val;
                else shifted = (uint16_t)(((val << shift) & mask) | (val >> (ppm - shift)));
                for (int k = 0; k < ppm; ++k) {
                    if (shifted & (1u << k)) result[k] |= (uint16_t)(1u << i);
                }
            }
            return result;
        }

        std::vector<uint8_t> LoRaDemodModule::hamming_decode(std::vector<uint16_t> codewords, int rdd)
        {
            int n = (int)codewords.size();
            std::vector<uint8_t> p2(n), p3(n), p5(n);
            for (int i = 0; i < n; ++i) {
                uint16_t cw = codewords[i];
                p2[i] = xorbits16(cw, 0x4b);
                p3[i] = xorbits16(cw, 0x17);
                p5[i] = xorbits16(cw, 0x2e);
            }
            if (rdd == 7 || rdd == 8) {
                for (int i = 0; i < n; ++i) {
                    int parity = p2[i] * 4 + p3[i] * 2 + p5[i];
                    int fix = 0;
                    if (parity == 3) fix = 4;
                    else if (parity == 5) fix = 8;
                    else if (parity == 6) fix = 1;
                    else if (parity == 7) fix = 2;
                    codewords[i] ^= (uint16_t)fix;
                }
            }
            std::vector<uint8_t> out(n);
            for (int i = 0; i < n; ++i) out[i] = (uint8_t)(codewords[i] & 0xf);
            return out;
        }

        int LoRaDemodModule::calc_sym_num(bool has_header, int payload_len, int cr, bool enable_crc, int sf, bool ldro)
        {
            int total = 2 * payload_len - sf + 7;
            if (enable_crc) total += 4;
            if (!has_header) total -= 5;
            int per_sym = sf;
            if (ldro) per_sym -= 2;
            int result = (int)std::ceil((double)total / (double)per_sym) * (cr + 4);
            return 8 + std::max(0, result);
        }

        void LoRaDemodModule::emit_payload(const uint8_t *data, int len)
        {
            if (input_data_type == DATA_DSP_STREAM && output_fifo) {
                output_fifo->write((uint8_t *)data, len);
            }

            if (!payload_out_hex.is_open()) {
                std::string path = d_output_file_hint + ".hex.txt";
                payload_out_hex.open(path, std::ios::app);
                if (payload_out_hex.is_open())
                    d_output_files.push_back(path);
            }
            if (payload_out_hex.is_open()) {
                char buf[8];
                for (int i = 0; i < len; ++i) {
                    snprintf(buf, sizeof(buf), "%02X ", data[i]);
                    payload_out_hex << buf;
                }
                payload_out_hex << "\n";
                payload_out_hex.flush();
            }

            if (!payload_out_bin.is_open()) {
                std::string path = d_output_file_hint + ".bin";
                payload_out_bin.open(path, std::ios::binary | std::ios::app);
                if (payload_out_bin.is_open())
                    d_output_files.push_back(path);
            }
            if (payload_out_bin.is_open()) {
                payload_out_bin.write((const char *)data, len);
                payload_out_bin.flush();
            }
        }

        void LoRaDemodModule::process()
        {
            try {
                stop_requested.store(false);
                reset_sync_state();

                if (!fft_in || !fft_out || !fft_plan) {
                    logger->error("LoRa Demod process() aborted: FFTW was not correctly initialized.");
                    return;
                }

                if (input_data_type == DATA_FILE && file_source) {
                    filesize = file_source->getFilesize();
                    file_source->start();
                } else {
                    filesize = 0;
                }

                std::shared_ptr<dsp::stream<complex_t>> in_stream = (input_data_type == DATA_FILE) ? file_source->output_stream : input_stream;
                if (!in_stream) return;

                time_t lastTime = 0;

                while (demod_should_run() && !stop_requested.load()) {
                    int n = in_stream->read();
                    if (n < 0) break;

                    if (n > 0 && in_stream->readBuf) {
                        complex_t *buf = in_stream->readBuf;
                        
                        // 降低 SNR 和 UI 刷新率，避免高 CPU 占用
                        block_count++;
                        if (block_count % 8 == 0) {
                            snr_estimator.update(buf, n);
                            current_snr = snr_estimator.snr();
                            if (current_snr > peak_snr) peak_snr = current_snr;
                            avg_snr = (avg_snr * total_snr_samples + current_snr) / (total_snr_samples + 1);
                            total_snr_samples++;

                            for (int i = 0; i < n; i += 64) { // 每 64 个样本推入一次星座图
                                complex_t s = buf[i];
                                if (s.real*s.real + s.imag*s.imag > 1e-12) {
                                    constellation.pushComplex(&s, 1);
                                }
                            }
                        }

                        for (int i = 0; i < n; i++) {
                            complex_t s = buf[i];
                            dc_state.real = s.real * (1.0f - dc_alpha) + dc_state.real * dc_alpha;
                            dc_state.imag = s.imag * (1.0f - dc_alpha) + dc_state.imag * dc_alpha;
                            s.real -= dc_state.real;
                            s.imag -= dc_state.imag;
                            sample_buffer.push_back(s);
                        }
                    }
                    in_stream->flush();

                    size_t overlap = (size_t)preamble_len * sample_num + 16 * sample_num;
                    while (true) {
                        size_t pkt_start;
                        try {
                            pkt_start = detect(sample_buffer, read_pos);
                        } catch (...) {
                            if (sample_buffer.size() > overlap) read_pos = sample_buffer.size() - overlap;
                            else read_pos = 0;
                            break; 
                        }

                        if (pkt_start + (size_t)(5 * sample_num) > sample_buffer.size()) {
                            read_pos = pkt_start; break;
                        }

                        SyncResult sr;
                        try {
                            sr = sync(sample_buffer, pkt_start);
                        } catch (...) {
                            read_pos = pkt_start + sample_num; continue;
                        }

                        display_freq = sr.cfo;

                        size_t ns = sr.new_start;
                        if (ns + (size_t)(8 * sample_num) > sample_buffer.size()) {
                            read_pos = pkt_start; break;
                        }

                        std::vector<uint16_t> hsym(8);
                        for (int i = 0; i < 8; ++i) {
                            auto r = dechirp(sample_buffer, ns + (size_t)i * sample_num);
                            hsym[i] = (uint16_t)peak_idx_to_symbol(r.second, sr.preamble_bin);
                        }

                        auto comp = dynamic_compensation(hsym, sr.cfo);
                        auto gc = gray_coding(comp);
                        std::vector<uint16_t> first8(gc.begin(), gc.begin() + std::min<size_t>(8, gc.size()));
                        auto cw = diag_deinterleave(first8, sf - 2);
                        auto nibbles = hamming_decode(cw, 8);

                        if (nibbles.size() < 5) { read_pos = ns + 7 * sample_num; continue; }
                        
                        int p_len = nibbles[0] * 16 + nibbles[1];
                        bool crc_en = (nibbles[2] & 1) != 0;
                        int cr_val = nibbles[2] >> 1;
                        
                        std::vector<uint8_t> cdata(12);
                        for (int i = 0; i < 3; ++i)
                            for (int j = 0; j < 4; ++j)
                                cdata[i * 4 + j] = (uint8_t)((nibbles[i] >> (3 - j)) & 1);
                        auto cs = calc_checksum(cdata);
                        bool h_valid = (cs[0] == (nibbles[3] & 1)) &&
                                       (cs[1] == ((nibbles[4] >> 3) & 1)) &&
                                       (cs[2] == ((nibbles[4] >> 2) & 1)) &&
                                       (cs[3] == ((nibbles[4] >> 1) & 1)) &&
                                       (cs[4] == (nibbles[4] & 1));

                        if (!h_valid) { read_pos = ns + 7 * sample_num; continue; }

                        payload_len = p_len;
                        coding_rate = cr_val;
                        header_has_crc = crc_en;
                        ldro = false;
                        if (2.0 * (1 << sf) / bw_hz > 16e-3) ldro = true;

                        int sym_num = calc_sym_num(true, payload_len, coding_rate, header_has_crc, sf, ldro);
                        if (ns + (size_t)sym_num * sample_num > sample_buffer.size()) {
                            read_pos = pkt_start; break;
                        }

                        std::vector<uint16_t> syms(sym_num);
                        for (int i = 0; i < 8; ++i) syms[i] = hsym[i];
                        for (int i = 8; i < sym_num; ++i) {
                            auto r = dechirp(sample_buffer, ns + (size_t)i * sample_num);
                            syms[i] = (uint16_t)peak_idx_to_symbol(r.second, sr.preamble_bin);
                        }

                        auto comp_all = dynamic_compensation(syms, sr.cfo);
                        auto gc_all = gray_coding(comp_all);

                        std::vector<uint8_t> nib_all(nibbles.begin(), nibbles.begin() + 5);
                        int rdd = coding_rate + 4;
                        for (size_t i = 8; i + (size_t)rdd <= gc_all.size(); i += rdd) {
                            int ppm = sf;
                            if (ldro) ppm -= 2;
                            std::vector<uint16_t> chunk(gc_all.begin() + i, gc_all.begin() + i + rdd);
                            auto c_chunk = diag_deinterleave(chunk, ppm);
                            auto nb = hamming_decode(c_chunk, rdd);
                            nib_all.insert(nib_all.end(), nb.begin(), nb.end());
                        }

                        if (nib_all.size() % 2 == 1) nib_all.push_back(0);
                        int data_len = (int)nib_all.size() / 2;
                        std::vector<uint8_t> data(data_len);
                        for (int i = 0; i < data_len; ++i) {
                            data[i] = (uint8_t)((nib_all[2 * i + 1] << 4) | nib_all[2 * i]);
                        }

                        bool crc_ok = false;
                        if (header_has_crc) {
                            std::vector<uint8_t> just(data.begin(), data.begin() + std::min<size_t>(payload_len, data.size()));
                            dewhiten(just);
                            auto checksum = calc_crc(just);
                            if (payload_len + 1 < (int)data.size()) {
                                if (checksum.first == data[payload_len] && checksum.second == data[payload_len + 1]) {
                                    crc_ok = true;
                                }
                            }
                            for (int i = 0; i < payload_len && i < (int)just.size(); ++i) data[i] = just[i];
                        } else {
                            dewhiten(data);
                            crc_ok = true;
                        }

                        if (crc_ok) {
                            packets_ok++;
                            logger->info("LoRa Packet OK! len=%d cfo=%.1f", payload_len, sr.cfo);
                            emit_payload(data.data(), payload_len);
                        } else {
                            packets_crc_fail++;
                        }

                        read_pos = ns + (size_t)sym_num * sample_num;
                        size_t back_len = (size_t)preamble_len * sample_num;
                        if (read_pos > back_len) read_pos -= back_len;
                        else read_pos = 0;
                    }

                    if (read_pos > sample_buffer.size()) read_pos = sample_buffer.size();
                    
                    if (read_pos > (size_t)sample_num * 64) {
                        sample_buffer.erase(sample_buffer.begin(), sample_buffer.begin() + read_pos);
                        read_pos = 0;
                    }

                    if (input_data_type == DATA_FILE && file_source)
                        progress = file_source->getPosition();

                    if (time(NULL) % 10 == 0 && lastTime != time(NULL)) {
                        lastTime = time(NULL);
                        if (input_data_type == DATA_FILE && filesize > 0) {
                            double pct = round(((double)progress / (double)filesize) * 1000.0) / 10.0;
                            logger->info("Progress " + std::to_string(pct) + "%");
                        } else {
                            logger->info("Live Mode: Packets OK=%d, CRC fail=%d, SNR=%.1f dB", packets_ok, packets_crc_fail, current_snr);
                        }
                    }
                }

                if (input_data_type == DATA_FILE && file_source)
                    file_source->stop();

                if (payload_out_bin.is_open()) payload_out_bin.close();
                if (payload_out_hex.is_open()) payload_out_hex.close();
            } catch (const std::exception &e) {
                logger->error("LoRa Demod thread caught exception: %s", e.what());
            } catch (...) {
                logger->error("LoRa Demod thread caught unknown exception");
            }
        }

        void LoRaDemodModule::reset_sync_state()
        {
            read_pos = 0;
            sample_buffer.clear();
            preamble_bins.clear();
            current_snr = 0;
            peak_snr = 0;
            avg_snr = 0;
            total_snr_samples = 0;
        }

        void LoRaDemodModule::drawUI(bool window)
        {
            ImGui::Begin("Generic LoRa Demod", NULL, window ? 0 : NOWINDOW_FLAGS);

            ImGui::BeginGroup();
            constellation.draw();
            ImGui::EndGroup();

            ImGui::SameLine();

            ImGui::BeginGroup();
            {
                ImGui::Button("Signal", {200 * ui_scale, 20 * ui_scale});
                
                ImGui::Text("Freq : ");
                ImGui::SameLine();
                ImGui::TextColored(style::theme.orange, "%.1f Hz", display_freq);

                ImGui::Text("SNR : ");
                ImGui::SameLine();
                if (std::isnan(current_snr)) {
                    ImGui::TextColored(style::theme.red, "NaN");
                } else if (current_snr > 30.0f) {
                    ImGui::TextColored(style::theme.green, "inf");
                } else if (current_snr > 5.0f) {
                    ImGui::TextColored(style::theme.green, "%.1f dB", current_snr);
                } else if (current_snr < 2.0f) {
                    ImGui::TextColored(style::theme.red, "%.1f dB", current_snr);
                } else {
                    ImGui::TextColored(style::theme.yellow, "%.1f dB", current_snr);
                }

                ImGui::Text("Peak SNR: %.1f dB", peak_snr);
                ImGui::Text("Avg SNR : %.1f dB", avg_snr);
                
                snr_plot.draw(current_snr, peak_snr);
            }
            ImGui::EndGroup();

            ImGui::Text("Packets OK: %d   CRC fail: %d", packets_ok, packets_crc_fail);

            if (input_data_type == DATA_FILE && filesize > 0)
                ImGui::ProgressBar((double)progress / (double)filesize, ImVec2(ImGui::GetContentRegionAvail().x, 20 * ui_scale));

            drawStopButton();
            ImGui::End();
        }

        std::string LoRaDemodModule::getID() { return "lora_demod"; }

        std::vector<std::string> LoRaDemodModule::getParameters()
        {
            return {"samplerate", "sf", "bw", "preamble_len", "zero_padding_ratio", "baseband_format"};
        }

        std::shared_ptr<ProcessingModule> LoRaDemodModule::getInstance(std::string input_file, std::string output_file_hint, nlohmann::json parameters)
        {
            return std::make_shared<LoRaDemodModule>(input_file, output_file_hint, parameters);
        }
    }
}