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

            // Butterworth Lowpass Filter State (SOS structure)
            struct SOSFilter
            {
                std::vector<std::vector<double>> sos; // [section][6]: b0, b1, b2, a0, a1, a2
                std::vector<std::vector<double>> states; // [section][2]: z1, z2
                void init(double cutoff, double fs, int order = 10);
                complex_t process(complex_t sample);
            } lpf_filter;

            void drawUI(bool window) override;
            void process() override;

            // LoRa DSP & Decoding Helpers
            void generate_chirp(int sf_val, std::vector<complex_t> &chirp_out, std::vector<complex_t> &inv_chirp_out);
            inthamming_decode(uint8_t codeword, int rdd, int &corrected_errs);
            void diag_deinterleave(const std::vector<uint8_t> &input_bits, std::vector<uint8_t> &output_bits, int sf_val, int cr, int num_symbols);
            uint16_t calc_crc_hqx(const uint8_t *data, size_t len);

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
