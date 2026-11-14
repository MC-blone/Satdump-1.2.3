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

        BiquadCascade::BiquadCascade(double fc, double fs, int order) {
            const double PI = std::acos(-1.0);
            double w0 = 2.0 * PI * fc / fs;
            double cos_w0 = std::cos(w0);
            double sin_w0 = std::sin(w0);
            int num_sections = order / 2;
            for (int k = 0; k < num_sections; ++k) {
                double Q = 1.0 / (2.0 * std::sin(PI * (2 * k + 1) / (2.0 * order)));
                double alpha = sin_w0 / (2.0 * Q);
                double a0 = 1.0 + alpha;
                b0[k] = ((1 - cos_w0) / 2) / a0;
                b1[k] = (1 - cos_w0) / a0;
                b2[k] = ((1 - cos_w0) / 2) / a0;
                a1[k] = (-2 * cos_w0) / a0;
                a2[k] = (1 - alpha) / a0;
                z1_r[k] = z2_r[k] = z1_i[k] = z2_i[k] = 0;
            }
        }

        complex_t BiquadCascade::process(complex_t x) {
            double r = x.real, i = x.imag;
            for (int k = 0; k < 5; ++k) {
                double yr = b0[k] * r + z1_r[k];
                z1_r[k] = b1[k] * r - a1[k] * yr + z2_r[k];
                z2_r[k] = b2[k] * r - a2[k] * yr;
                r = yr;

                double yi = b0[k] * i + z1_i[k];
                z1_i[k] = b1[k] * i - a1[k] * yi + z2_i[k];
                z2_i[k] = b2[k] * i - a2[k] * yi;
                i = yi;
            }
            return complex_t((float)r, (float)i);
        }

        LoRaDemodModule::LoRaDemodModule(std::string input_file, std::string output_file_hint, nlohmann::json parameters)
            : BaseDemodModule(input_file, output_file_hint, parameters)
        {
            if (parameters.count("sf")) sf = parameters["sf"].get<int>();
            if (parameters.count("bw")) bw_hz = parameters["bw"].get<int>();
            if (parameters.count("preamble_len")) preamble_len = parameters["preamble_len"].get<int>();

            if (sf < 7 || sf > 12) sf = 9;
            if (bw_hz <= 0) bw_hz = 812500;
            if (preamble_len <= 1) preamble_len = 6;
            
            zero_padding_ratio = 5;

            d_parameters.erase("max_sps");
            d_parameters.erase("min_sps");
            d_parameters.erase("custom_samplerate");
            
            MIN_SPS = 0.5f;
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
            
            if (final_samplerate <= 0) final_samplerate = d_samplerate;

            sample_num = (int)std::round(std::pow(2.0, sf) * (double)final_samplerate / (double)bw_hz);
            if (sample_num <= 0) sample_num = 1291;
            
            bin_num = zero_padding_ratio * (1 << sf);
            fft_len = sample_num * zero_padding_ratio;

            init_chirps((double)final_samplerate);
            lpf = std::make_unique<BiquadCascade>(bw_hz / 2.0, (double)final_samplerate, 10);

            fft_in = (fftwf_complex *)fftwf_malloc(sizeof(fftwf_complex) * fft_len);
            fft_out = (fftwf_complex *)fftwf_malloc(sizeof(fftwf_complex) * fft_len);
            mag_buffer.resize(fft_len);

            if (!fft_in || !fft_out) return;
            
            memset(fft_in, 0, sizeof(fftwf_complex) * fft_len);
            fft_plan = fftwf_plan_dft_1d(fft_len, fft_in, fft_out, FFTW_FORWARD, FFTW_ESTIMATE);
            if (!fft_plan) return;

            sample_buffer.reserve((size_t)sample_num * 128);

            payload_out_hex.open(d_output_file_hint + ".hex.txt", std::ios::app);
            if (payload_out_hex.is_open()) d_output_files.push_back(d_output_file_hint + ".hex.txt");
            payload_out_bin.open(d_output_file_hint + ".bin", std::ios::app);
            if (payload_out_bin.is_open()) d_output_files.push_back(d_output_file_hint + ".bin");
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
            if (input_data_type == DATA_FILE && file_source) file_source->stop();
        }

        std::pair<float, int> LoRaDemodModule::dechirp(const std::vector<complex_t>& buf, size_t pos, bool is_up)
        {
            if (pos >= buf.size() || !fft_in || !fft_plan) return {0.0f, 0};
            const auto& chp = is_up ? chirp_down : chirp_up;
            size_t signal_len = std::min((size_t)sample_num, buf.size() - pos);
            
            memset(fft_in, 0, sizeof(fftwf_complex) * fft_len);
            volk_32fc_x2_multiply_32fc((lv_32fc_t*)fft_in, (const lv_32fc_t*)&buf[pos], (const lv_32fc_t*)chp.data(), signal_len);
            
            fftwf_execute(fft_plan);
            fft_calls++;
            
            volk_32fc_magnitude_squared_32f(mag_buffer.data(), (const lv_32fc_t*)fft_out, fft_len);

            float best_mag = 0.0f;
            int best = 0;
            for (int i = 0; i < bin_num; ++i) {
                float m = mag_buffer[i] + mag_buffer[fft_len - bin_num + i];
                if (m > best_mag) { best_mag = m; best = i; }
            }
            return {std::sqrt(best_mag), best};
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
            size_t required = (size_t)sample_num * preamble_len + 1;
            if (buf.size() < required) throw std::runtime_error("no preamble");

            int consecutive = 0;
            int last_peak = 0;

            while (start < buf.size() - required) {
                if (consecutive >= preamble_len - 1) {
                    long long back = (long long)std::round((double)last_peak / zero_padding_ratio * 2.0);
                    long long p = (long long)start - back;
                    if (p < 0) p = 0;
                    return (size_t)p;
                }

                auto r = dechirp(buf, start, true);
                int peak = r.second;

                if (consecutive > 0) {
                    int diff = (last_peak - peak) % bin_num;
                    if (diff < 0) diff += bin_num;
                    if (diff > bin_num / 2) diff = bin_num - diff;

                    if (diff <= zero_padding_ratio * 10) consecutive++;
                    else consecutive = 1;
                } else {
                    consecutive = 1;
                }

                last_peak = peak;
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

            if (payload_out_hex.is_open()) {
                char buf[8];
                for (int i = 0; i < len; ++i) {
                    snprintf(buf, sizeof(buf), "%02X ", data[i]);
                    payload_out_hex << buf;
                }
                payload_out_hex << "\n";
                payload_out_hex.flush();
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

                if (!fft_in || !fft_out || !fft_plan) return;

                if (input_data_type == DATA_FILE && file_source) {
                    filesize = file_source->getFilesize();
                    file_source->start();
                } else {
                    filesize = 0;
                }

                std::shared_ptr<dsp::stream<complex_t>> in_stream = (input_data_type == DATA_FILE) ? file_source->output_stream : input_stream;
                if (!in_stream) return;

                time_t lastTime = 0;
                int ui_counter = 0;

                while (demod_should_run() && !stop_requested.load()) {
                    int n = in_stream->read();
                    if (n < 0) break;

                    if (n > 0 && in_stream->readBuf) {
                        complex_t *buf = in_stream->readBuf;
                        
                        ui_counter++;
                        if (ui_counter % 8 == 0) {
                            snr_estimator.update(buf, n);
                            snr = snr_estimator.snr();
                            if (snr > peak_snr) peak_snr = snr;
                        }

                        for (int i = 0; i < n; i++) {
                            complex_t s = buf[i];
                            
                            // IQ 反转（如果在 UI 上没勾的话，这里根据实际情况决定。这里默认启用反转）
                            s.imag = -s.imag;
                            
                            dc_state.real = s.real * (1.0f - 0.999f) + dc_state.real * 0.999f;
                            dc_state.imag = s.imag * (1.0f - 0.999f) + dc_state.imag * 0.999f;
                            s.real -= dc_state.real;
                            s.imag -= dc_state.imag;
                            
                            s = lpf->process(s);
                            sample_buffer.push_back(s);

                            if (ui_counter % 8 == 0 && i % 64 == 0) {
                                constellation.pushComplex(&s, 1);
                            }
                        }
                    }
                    in_stream->flush();

                    size_t overlap = (size_t)preamble_len * sample_num + 16 * sample_num;
                    while (true) {
                        size_t pkt_start;
                        try {
                            pkt_start = detect(sample_buffer, read_pos);
                            preambles_detected++;
                        } catch (...) {
                            if (sample_buffer.size() > overlap) read_pos = sample_buffer.size() - overlap;
                            else read_pos = 0;
                            break; 
                        }

                        if (pkt_start + (size_t)(5 * sample_num) > sample_buffer.size()) {
                            read_pos = pkt_start; break;
                        }

                        size_t sync_required = (size_t)(preamble_len + 3) * sample_num;
                        if (pkt_start > sample_buffer.size() || sample_buffer.size() - pkt_start < sync_required) {
                            read_pos = pkt_start; 
                            break;
                        }

                        SyncResult sr;
                        try {
                            sr = sync(sample_buffer, pkt_start);
                            sync_found++;
                        } catch (const std::exception &) {
                            sync_failed++;
                            read_pos = pkt_start + sample_num; continue;
                        }

                        display_freq = sr.cfo;

                        size_t ns = sr.new_start;
                        if (ns + (size_t)(8 * sample_num) > sample_buffer.size()) {
                            read_pos = pkt_start; break;
                        }

                        // LDRO 状态必须在解 Header 之前设置
                        ldro = ((double)(1 << sf) / (double)bw_hz) > 16e-3;

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

                        if (nibbles.size() < 5) { header_fail++; read_pos = ns + 7 * sample_num; continue; }
                        
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

                        if (!h_valid) { header_fail++; read_pos = ns + 7 * sample_num; continue; }

                        if (cr_val < 1 || cr_val > 4) {
                            header_fail++;
                            read_pos = ns + 7 * sample_num;
                            continue;
                        }

                        headers_valid++;
                        payload_len = p_len;
                        coding_rate = cr_val;
                        header_has_crc = crc_en;

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

                        // 【核心修复 1】Payload 中不包含 Header 半字节
                        std::vector<uint8_t> nib_all;
                        int rdd = coding_rate + 4;
                        for (size_t i = 8; i + (size_t)rdd <= gc_all.size(); i += rdd) {
                            int ppm = ldro ? sf - 2 : sf;
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

                        // 【核心修复 2】正确的 CRC 路径
                        bool crc_ok = false;
                        std::vector<uint8_t> payload;
                        size_t required = (size_t)payload_len + (header_has_crc ? 2u : 0u);

                        if (data.size() >= required) {
                            if (header_has_crc) {
                                std::vector<uint8_t> frame(data.begin(), data.begin() + required);
                                dewhiten(frame);
                                uint16_t crc = crc_hqx(frame.data(), (size_t)payload_len);
                                uint8_t crc_low = (uint8_t)(crc & 0xff);
                                uint8_t crc_high = (uint8_t)(crc >> 8);
                                crc_ok = frame[payload_len] == crc_low && frame[payload_len + 1] == crc_high;
                                payload.assign(frame.begin(), frame.begin() + payload_len);
                            } else {
                                payload.assign(data.begin(), data.begin() + payload_len);
                                dewhiten(payload);
                                crc_ok = true;
                            }
                        } else {
                            logger->warning("LoRa decoded payload too short: have=%d need=%d", (int)data.size(), (int)required);
                        }

                        if (crc_ok) {
                            packets_ok++;
                            logger->info("LoRa Packet OK! len=%d cfo=%.1f", payload_len, sr.cfo);
                            emit_payload(payload.data(), (int)payload.size());
                        } else {
                            if (header_has_crc) packets_crc_fail++;
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
                    
                    if (sample_buffer.size() > 2000000) {
                        size_t drop = sample_buffer.size() - 1000000;
                        if (read_pos > drop) read_pos -= drop;
                        else read_pos = 0;
                        sample_buffer.erase(sample_buffer.begin(), sample_buffer.begin() + drop);
                    }

                    if (input_data_type == DATA_FILE && file_source)
                        progress = file_source->getPosition();

                    if (time(NULL) % 5 == 0 && lastTime != time(NULL)) {
                        lastTime = time(NULL);
                        if (input_data_type == DATA_FILE && filesize > 0) {
                            double pct = round(((double)progress / (double)filesize) * 1000.0) / 10.0;
                            logger->info("Progress " + std::to_string(pct) + "%");
                        }
                    }
                }

                if (input_data_type == DATA_FILE && file_source) file_source->stop();
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
            
            packets_ok = 0;
            packets_crc_fail = 0;
            preambles_detected = 0;
            sync_found = 0;
            sync_failed = 0;
            headers_valid = 0;
            header_fail = 0;
            fft_calls = 0;
        }

        void LoRaDemodModule::drawUI(bool window)
        {
            // 直接调用基类绘制，完美保留 SatDump 原生的 SNR 和 FFT 曲线
            BaseDemodModule::drawUI(window);
            
            // 在这里追加我们自己需要的统计信息（利用原生窗口的位置）
            ImGui::Begin(name.c_str(), NULL, window ? 0 : NOWINDOW_FLAGS);
            ImGui::Text("Preamble: %llu  Sync: %llu  Header: %llu",
                (unsigned long long)preambles_detected.load(),
                (unsigned long long)sync_found.load(),
                (unsigned long long)headers_valid.load());

            ImGui::Text("Sync fail: %llu  Header fail: %llu",
                (unsigned long long)sync_failed.load(),
                (unsigned long long)header_fail.load());

            ImGui::Text("Packets OK: %d  CRC fail: %d  FFT calls: %llu",
                packets_ok.load(),
                packets_crc_fail.load(),
                (unsigned long long)fft_calls.load());
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