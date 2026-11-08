#pragma once

#include "core/module.h"
#include "modules/demod/module_demod_base.h"
#include "common/dsp/resamp/rational_resampler.h"
#include <fstream>
#include <atomic>
#include <mutex>
#include <vector>
#include <deque>
#include <fftw3.h>

namespace fengyun4
{
    namespace emwin
    {
        class LoRaDemodModule : public demod::BaseDemodModule
        {
        protected:
            int sf = 9;
            int bw_hz = 812500;
            int preamble_len = 6;
            int zero_padding_ratio = 10;
            bool resample_to_2x = true;

            int sample_num = 0;
            int bin_num = 0;
            int fft_len = 0;

            int last_pkts = 0;
            std::atomic<bool> stop_requested{false};
            std::mutex rsp_mtx;
            std::shared_ptr<dsp::RationalResamplerBlock<complex_t>> active_rsp;

            std::ofstream payload_out;

            // Butterworth 低通滤波器 (SOS 结构状态)
            struct SOSFilter
            {
                std::vector<std::vector<double>> sos;
                std::vector<std::vector<double>> states;
                void init(double cutoff, double fs, int order = 10);
                complex_t process(complex_t sample);
            } lpf_filter;

            void drawUI(bool window) override;
            void process() override;

            // LoRa 核心 DSP 与解码算法辅助函数
            void generate_chirp(bool up, int sf_val, double bw, double fs, std::vector<complex_t> &out);
            void lowpass_filter(const std::vector<complex_t> &in, std::vector<complex_t> &out);
            std::pair<float, int> dechirp(const std::vector<complex_t> &signal, size_t x, bool is_up = true);
            int detect_preamble(const std::vector<complex_t> &signal, size_t &start_idx);
            bool sync_packet(const std::vector<complex_t> &signal, size_t &start_idx, int &preamble_bin, float &cfo);
            int peak_idx_to_symbol(int peak_idx, int preamble_bin);
            
            void dynamic_compensation(std::vector<uint16_t> &data, float cfo, double rf_freq, bool ldo);
            void gray_coding(std::vector<float> &data_float, std::vector<uint16_t> &out_symbols, bool ldo);
            void diag_deinterleave(const std::vector<uint16_t> &input_symbols, std::vector<uint16_t> &output_codewords, int ppm);
            std::vector<uint8_t> hamming_decode(const std::vector<uint16_t> &codewords, int rdd, bool hamming_en);
            bool parse_header(const std::vector<uint16_t> &symbols, int &payload_len, int &cr, bool &crc);
            bool parse_header_inner(const std::vector<uint8_t> &nibbles, int &payload_len, int &cr, bool &crc);
            void dewhiten(std::vector<uint8_t> &data);
            uint16_t calc_crc_hqx(const uint8_t *data, size_t len);
            int calc_sym_num(bool has_header, int payload_len, int cr, bool enable_crc, int spreading_factor, bool ldo);

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
