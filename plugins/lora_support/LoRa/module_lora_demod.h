#pragma once

#include "core/module.h"
#include "modules/demod/module_demod_base.h"
#include "common/dsp/resamp/rational_resampler.h"
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
        // 单级双二阶（Direct Form II Transposed），对复数 I/Q 分别处理
        struct Biquad
        {
            float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f, a1 = 0.0f, a2 = 0.0f;
            float z1r = 0.0f, z2r = 0.0f, z1i = 0.0f, z2i = 0.0f;

            complex_t process(complex_t x)
            {
                float yr = b0 * x.real + z1r;
                z1r = b1 * x.real - a1 * yr + z2r;
                z2r = b2 * x.real - a2 * yr;

                float yi = b0 * x.imag + z1i;
                z1i = b1 * x.imag - a1 * yi + z2i;
                z2i = b2 * x.imag - a2 * yi;

                return complex_t(yr, yi);
            }

            void reset() { z1r = z2r = z1i = z2i = 0.0f; }
        };

        class LoRaDemodModule : public demod::BaseDemodModule
        {
        protected:
            // ---------- 配置参数 ----------
            int sf = 9;
            int bw_hz = 812500;
            int preamble_len = 6;
            int zero_padding_ratio = 10;
            bool resample_to_2x = true;

            // ---------- 派生尺寸 ----------
            int sample_num = 512;   // 2^SF
            int bin_num = 5120;     // zero_padding_ratio * sample_num
            int fft_len = 5120;     // zero_padding_ratio * sample_num

            // ---------- FFTW ----------
            fftwf_complex *fft_in = nullptr;
            fftwf_complex *fft_out = nullptr;
            fftwf_plan fft_plan = nullptr;

            // ---------- Chirp ----------
            std::vector<complex_t> chirp_up;
            std::vector<complex_t> chirp_down;

            // ---------- Butterworth 10 阶 LPF（截止 BW/2，作用于 rsp 输出） ----------
            std::vector<Biquad> butter;
            bool butter_ready = false;

            // ---------- 连续输入缓冲 ----------
            std::vector<complex_t> sample_buffer;   // rsp 输出 + LPF 后的滚动 IQ
            size_t read_pos = 0;

            // ---------- 同步状态机 ----------
            enum class SyncState { SEARCH_PREAMBLE, CHECK_SFD, DECODE_HEADER, DECODE_PAYLOAD };
            SyncState sync_state = SyncState::SEARCH_PREAMBLE;

            std::vector<int> preamble_bins;
            int preamble_bin = 0;
            int cfo_bin = 0;
            double cfo_hz = 0.0;
            int sfo_accum = 0;
            int symbols_since_sync = 0;
            double data_symbol_offset = 2.25; // 2.25 或 1.25（由同步结果决定）

            // ---------- Header / Payload ----------
            int payload_len = 0;
            int cr = 1;                 // 1..4 → 4/5..4/8
            bool ldro = false;          // Low Data Rate Optimization
            bool header_has_crc = true;

            std::vector<uint16_t> header_symbols;
            std::vector<uint16_t> payload_symbols;
            std::vector<uint8_t> decoded_payload;

            // ---------- 输出 ----------
            std::ofstream payload_out;

            // ---------- 停止机制（必须保留） ----------
            std::atomic<bool> stop_requested{false};
            std::mutex rsp_mtx;
            std::shared_ptr<dsp::RationalResamplerBlock<complex_t>> active_rsp;

            // ---------- 统计 ----------
            int packets_ok = 0;
            int packets_crc_fail = 0;
            int last_syms = 0;

            // ---------- 解码链 ----------
            void init_chirps();
            void init_butterworth();
            int  dechirp_and_peak(const complex_t *samples, int n);
            bool try_detect_preamble();
            bool check_sfd_and_fine_sync();
            bool decode_header();
            bool decode_payload();
            void reset_sync_state();
            void compact_buffer();

            // ---------- 解码原语 ----------
            static uint16_t gray_decode(uint16_t g);
            static std::vector<uint16_t> diag_deinterleave(const std::vector<uint16_t> &syms,
                                                          int ppm, int rdd);
            static uint8_t hamming_decode_rdd(uint16_t cw, int rdd);
            static void dewhiten(uint8_t *data, int len);
            static uint16_t lora_crc16(const uint8_t *data, int len);
            static inline int parity8(uint8_t x);

            // ---------- 输出 ----------
            void emit_payload(const uint8_t *data, int len);

            void drawUI(bool window) override;
            void process() override;

        public:
            LoRaDemodModule(std::string input_file, std::string output_file_hint,
                            nlohmann::json parameters);
            ~LoRaDemodModule();

            void stop() override;
            void init() override;

            static std::string getID();
            std::string getIDM() override { return getID(); }
            static std::vector<std::string> getParameters();
            static std::shared_ptr<ProcessingModule> getInstance(
                std::string input_file, std::string output_file_hint,
                nlohmann::json parameters);
        };
    }
}