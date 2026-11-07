#pragma once

#include "core/module.h"
#include "modules/demod/module_demod_base.h"
#include "common/dsp/resamp/rational_resampler.h"
#include <fstream>
#include <vector>
#include <complex>

namespace fengyun4
{
    namespace emwin
    {
        class LoRaDemodModule : public demod::BaseDemodModule
        {
        protected:
            int8_t *soft_buffer = nullptr;
            int sf = 9;
            int bw_hz = 812500;
            int fft_size = 512;
            int last_syms = 0;
            std::ofstream data_out;

            void drawUI(bool window) override;
            void process() override;

        public:
            LoRaDemodModule(std::string input_file, std::string output_file_hint, nlohmann::json parameters);
            ~LoRaDemodModule();

            static std::string getID();
            virtual std::string getIDM() { return getID(); }
            static std::vector<std::string> getParameters();
            static std::shared_ptr<ProcessingModule> getInstance(std::string input_file, std::string output_file_hint, nlohmann::json parameters);
        };
    }
}