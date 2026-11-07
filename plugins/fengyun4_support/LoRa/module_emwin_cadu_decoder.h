#pragma once

#include "core/module.h"
#include <fstream>
#include <vector>
#include <string>
#include <atomic>
#include <mutex>
#include <cstdint>

namespace fengyun4
{
    namespace emwin
    {
        class EmwinCaduDecoderModule : public ProcessingModule
        {
        protected:
            std::ifstream data_in;
            std::ofstream cadu_out;
            std::atomic<uint64_t> filesize{0};
            std::atomic<uint64_t> progress{0};

            int sf = 9;
            int total_syms = 0;
            int total_bits = 0;
            int total_cadu_bytes = 0;

            mutable std::mutex stat_mtx;

        public:
            EmwinCaduDecoderModule(std::string input_file, std::string output_file_hint, nlohmann::json parameters);
            ~EmwinCaduDecoderModule();
            void process() override;
            void drawUI(bool window) override;
            std::vector<ModuleDataType> getInputTypes() override;
            std::vector<ModuleDataType> getOutputTypes() override;

            static std::string getID();
            virtual std::string getIDM() { return getID(); }
            static std::vector<std::string> getParameters();
            static std::shared_ptr<ProcessingModule> getInstance(std::string input_file, std::string output_file_hint, nlohmann::json parameters);
        };
    }
}