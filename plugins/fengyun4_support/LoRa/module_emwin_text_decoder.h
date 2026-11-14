#pragma once

#include "core/module.h"
#include <fstream>
#include <mutex>
#include <vector>
#include <string>
#include <atomic>

namespace fengyun4
{
    namespace emwin
    {
        class EmwinTextDecoderModule : public ProcessingModule
        {
        protected:
            uint8_t *buffer = nullptr;
            std::ifstream data_in;
            std::atomic<uint64_t> filesize{0};
            std::atomic<uint64_t> progress{0};

            int sf = 9;
            int total_syms = 0;
            int valid_syms = 0;
            int file_seq = 0;

            std::mutex msg_mtx;
            std::vector<nlohmann::json> msg_history;
            int last_msg_count = 0;

            bool is_gui = false;

            void process_final_text(const std::string &text, double timestamp);

        public:
            EmwinTextDecoderModule(std::string input_file, std::string output_file_hint, nlohmann::json parameters);
            ~EmwinTextDecoderModule();
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