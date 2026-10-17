#pragma once

#include "core/module.h"
#include "common/codings/deframing/bpsk_ccsds_deframer.h"
#include "common/codings/viterbi/viterbi_3_4.h"
#include <fstream>

// ★ 引入计数需要的内容
#include "common/ccsds/ccsds_aos/demuxer.h"
#include "../instruments/avhrr/avhrr_reader.h"      // AVHRR 在 plugin 根目录
#include "instruments/iasi/iasi_imaging_reader.h"
namespace metop
{
    class MetOpAHRPTDecoderModule : public ProcessingModule
    {
    protected:
        int d_viterbi_outsync_after;
        float d_viterbi_ber_thresold;

        uint8_t *viterbi_out;
        int8_t *soft_buffer;

        std::ifstream data_in;
        std::ofstream data_out;
        std::atomic<uint64_t> filesize;
        std::atomic<uint64_t> progress;

        viterbi::Viterbi3_4 viterbi;
        deframing::BPSK_CCSDS_Deframer deframer;

        int errors[4];

        // ★ 新增：用于内部计数的 reader 和 demuxer
        AVHRRMetOpReader avhrr_reader;
        IASIIMGReader iasi_reader_img;
        ccsds::ccsds_aos::Demuxer demuxer_vcid9;
        ccsds::ccsds_aos::Demuxer demuxer_vcid10;

        // UI Stuff
        float ber_history[200];

    public:
        MetOpAHRPTDecoderModule(std::string input_file, std::string output_file_hint, nlohmann::json parameters);
        ~MetOpAHRPTDecoderModule();
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
} // namespace metop