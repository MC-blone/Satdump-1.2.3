#pragma once

#include "core/module.h"
#include "modules/demod/module_demod_base.h"
#include <fftw3.h>
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
        class LoRaDemodModule : public demod::BaseDemodModule
        {
        protected:
            // ---------- 配置参数 ----------
            int sf = 9;
            int bw_hz = 812500;
            int preamble_len = 6;
            int zero_padding_ratio = 10;

            // ---------- 派生尺寸 (在 init 中根据采样率动态计算) ----------
            int sample_num = 1291;
            int bin_num = 5120;
            int fft_len = 12910;

            // ---------- FFTW ----------
            fftwf_complex *fft_in = nullptr;
            fftwf_complex *fft_out = nullptr;
            fftwf_plan fft_plan = nullptr;

            // ---------- Chirp ----------
            std::vector<complex_t> chirp_up;
            std::vector<complex_t> chirp_down;

            // ---------- 连续输入缓冲 ----------
            std::vector<complex_t> sample_buffer;
            size_t read_pos = 0;

            // ---------- 同步与解码状态 ----------
            std::vector<int> preamble_bins;
            int preamble_bin = 0;
            double cfo_hz = 0.0;
            
            int payload_len = 0;
            int coding_rate = 1;
            bool header_has_crc = true;
            bool ldro = false;

            // 去直流状态
            complex_t dc_state = complex_t(0, 0);
            const float dc_alpha = 0.999f;

            // ---------- 输出文件 ----------
            std::ofstream payload_out_bin;
            std::ofstream payload_out_hex;

            // 停止机制
            std::atomic<bool> stop_requested{false};

            // ---------- 统计与 UI 状态 ----------
            int packets_ok = 0;
            int packets_crc_fail = 0;
            int last_syms = 0;
            
            // SNR 相关
            dsp::M2M4SNREstimator snr_estimator;
            float current_snr = 0;
            float peak_snr = 0;
            float avg_snr = 0;
            uint64_t total_snr_samples = 0;
            double display_freq = 0.0;

            // ---------- 内部方法 ----------
            void init_chirps(double fs);
            std::pair<float, int> dechirp(const std::vector<complex_t>& buf, size_t pos, bool is_up);
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