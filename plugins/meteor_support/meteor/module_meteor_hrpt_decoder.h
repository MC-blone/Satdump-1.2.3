#pragma once

#include "core/module.h"
#include <complex>
#include <thread>
#include <fstream>
#include "deframer.h"
#include "common/widgets/constellation.h"

// ★ 引入计数所需的头文件
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

        // UI Stuff
        widgets::ConstellationViewer constellation;

        // ★ 新增：Fix 状态管理
        bool use_fix = false;
        int consecutive_syncs = 0;
        time_t last_sync_time = 0;
        bool fix_status_idle = true;

        // ★ 计数与 BIS-M 提取相关的成员变量
        msumr::MSUMRReader msumr_reader;
        mtvza::MTVZAReader mtvza_reader, mtvza_reader2;
        bism::BISMReader bism_reader;

        def::SimpleDeframer msumr_deframer;
        def::SimpleDeframer mtvza_deframer;
        def::SimpleDeframer mtvza_deframer2;
        def::SimpleDeframer bism_deframer;

        std::ofstream bism_raw_out;
        std::ofstream bism_bin_out;

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