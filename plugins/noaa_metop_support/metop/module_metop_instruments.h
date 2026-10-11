#pragma once

#include "core/module.h"
#include "metop.h"
#include <fstream>

namespace metop
{
    namespace instruments
    {
        class MetOpInstrumentsDecoderModule : public ProcessingModule
        {
        protected:
            std::ifstream data_in;
            std::ofstream data_out;                    // ★ 新增：流模式保存 CADU
            std::atomic<uint64_t> filesize;
            std::atomic<uint64_t> progress;

            // ===== 原版 reader 全部保留 =====
            AVHRRMetOpReader avhrr_reader;
            MHSMetOpReader mhs_reader;
            ASCATMetOpReader ascat_reader;
            IASIMetOpReader iasi_reader;
            IASIIMGReader iasi_reader_img;
            AMSUMetOpReader amsu_reader;
            SEMMetOpReader sem_reader;
            GOMEMetOpReader gome_reader;
            AdminMsgReader admin_msg_reader;

            AVHRRToHpt *avhrr_to_hpt = nullptr;

            bool write_hpt;
            bool ignore_integrated_tle;

            // status flags
            instrument_status_t avhrr_status = DECODING;
            instrument_status_t mhs_status = DECODING;
            instrument_status_t ascat_status = DECODING;
            instrument_status_t iasi_status = DECODING;
            instrument_status_t iasi_img_status = DECODING;
            instrument_status_t amsu_status = DECODING;
            instrument_status_t gome_status = DECODING;
            instrument_status_t sem_status = DECODING;
            instrument_status_t admin_msg_status = DECODING;

            // ★ 新增：两个处理函数
            void processFile();     // 原 process() 内容
            void processStream();   // 流模式：保存 CADU + 计数

        public:
            MetOpInstrumentsDecoderModule(std::string input_file, std::string output_file_hint, nlohmann::json parameters);
            ~MetOpInstrumentsDecoderModule();
            void process();
            void drawUI(bool window);
            std::vector<ModuleDataType> getInputTypes();
            std::vector<ModuleDataType> getOutputTypes();

        public:
            static std::string getID();
            virtual std::string getIDM() { return getID(); };
            static std::vector<std::string> getParameters();
            static std::shared_ptr<ProcessingModule> getInstance(std::string input_file, std::string output_file_hint, nlohmann::json parameters);
        };
    } // namespace instruments
} // namespace metop