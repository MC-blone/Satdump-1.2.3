#pragma once

#include "core/module.h"
#include "modules/demod/module_demod_base.h"
#include <fftw3.h>
#include <volk/volk.h>
#include <fstream>
#include <atomic>
#include <mutex>
#include <vector>
#include <string>
#include <cstdint>

namespace fengyun4
{
    namespace emwin
    {
        struct BiquadCascade
        {
            double b0[5], b1[5], b2[5], a1[5], a2[5];
            double z1_r[5], z2_r[5], z1_i[5], z2_i[5];
            BiquadCascade(double fc, double fs, int order);
            complex_t process(complex_t x);
        };

        class LoRaDemodModule : public demod::BaseDemodModule
        {
        protected:
            int sf = 9;
            int bw_hz = 812500;
            int preamble_len = 6;
            double zero_padding_ratio = 10.0; // 这里保留为浮点
            
            // 新增：目标 SPS 和自适应缩放
            float target_sps = 2.0f;
            double fft_scale = 10.0;

            int sample_num = 1024; // 默认预期值，会在 init 中重新计算
            int bin_num = 5120;
            int fft_len = 10240;

            fftwf_complex *fft_in = nullptr;
            fftwf_complex *fft_out = nullptr;
            fftwf_plan fft_plan = nullptr;
            std::vector<float> mag_buffer;

            std::vector<complex_t> chirp_up;
            std::vector<complex_t> chirp_down;

            std::vector<complex_t> sample_buffer;
            size_t read_pos = 0;

            std::vector<int> preamble_bins;
            int preamble_bin = 0;
            
            int payload_len = 0;
            int coding_rate = 1;
            bool header_has_crc = true;
            bool ldro = false;

            complex_t dc_state;
            std::unique_ptr<BiquadCascade> lpf;

            std::ofstream payload_out_bin;
            std::ofstream payload_out_hex;

            std::atomic<bool> stop_requested{false};

            std::atomic<int> packets_ok{0};
            std::atomic<int> packets_crc_fail{0};
            std::atomic<uint64_t> preambles_detected{0};
            std::atomic<uint64_t> sync_found{0};
            std::atomic<uint64_t> sync_failed{0};
            std::atomic<uint64_t> headers_valid{0};
            std::atomic<uint64_t> header_fail{0};
            std::atomic<uint64_t> fft_calls{0};

            void init_chirps(double fs);
            std::pair<float, int> dechirp(const std::vector<complex_t>& buf, size_t pos, bool is_up = true);
            int peak_idx_to_symbol(int peak_idx, int pre_bin);
            size_t detect(const std::vector<complex_t>& buf, size_t start);
            
            struct SyncResult { size_t new_start; int preamble_bin; double cfo; };
            SyncResult sync(const std::vector<complex_t>& buf, size_t start);
            
            std::vector<double> dynamic_compensation(const std::vector<uint16_t>& data, double cfo);
            std::vector<uint16_t> gray_coding(const std::vector<double>& data);
            std::vector<uint16_t> diag_deinterleave(const std::vector<uint16_t>& syms, int ppm);
            std::vector<uint8_t> hamming_decode(std::vector<uint16_t> codewords, int rdd);
            int calc_sym_num(bool has_header, int payload_len, int cr, bool enable_crc, int sf, bool ldro);

            void emit_payload(const uint8_t *data, int len);
            void reset_sync_state();

            void drawUI(bool window) override;
            void process() override;

        public:
            LoRaDemodModule(std::string input_file, std::string output_file_hint, nlohmann::json parameters);
            ~LoRaDemodModule();

            void stop() override;
            void init() override;

            static std::string getID();
            std::string getIDM() override { return getID(); }
            static std::vector<std::string> getParameters();
            static std::shared_ptr<ProcessingModule> getInstance(std::string input_file, std::string output_file_hint, nlohmann::json parameters);
        };
    }
}