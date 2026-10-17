#pragma once

#include "core/module.h"
#include <complex>
#include <thread>
#include <fstream>
#include "deframer.h"
#include "common/widgets/constellation.h"

// ★ 新增引入
#include "common/simple_deframer.h"
#include "instruments/msumr/msumr_reader.h"
#include "instruments/mtvza/mtvza_reader.h"
#include "instruments/bism/bism_reader.h"

namespace meteor
{
    class METEORHRPTDecoderModule : public ProcessingModule
    {
    protected:
        std::shared_ptr<CADUDeframer> def;

        int8_t *soft_buffer;

        std::ifstream data_in;
        std::ofstream data_out;

        std::atomic<uint64_t> filesize;
        std::atomic<uint64_t> progress;

        // ★ 新增：用于计数的 Reader 和 Deframer
        msumr::MSUMRReader msumr_reader;
        mtvza::MTVZAReader mtvza_reader, mtvza_reader2;
        bism::BISMReader bism_reader;

        def::SimpleDeframer msumr_deframer;
        def::SimpleDeframer mtvza_deframer;
        def::SimpleDeframer mtvza_deframer2;
        def::SimpleDeframer bism_deframer;

        // ★ 新增：BIS-M 数据提取文件流
        std::ofstream bism_raw_out;
        std::ofstream bism_bin_out;

        // UI Stuff
        widgets::ConstellationViewer constellation;

    public:
        METEORHRPTDecoderModule(std::string input_file, std::string output_file_hint, nlohmann::json parameters);
        ~METEORHRPTDecoderModule();
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
} // namespace meteor