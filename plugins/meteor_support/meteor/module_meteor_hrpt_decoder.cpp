#include "module_meteor_hrpt_decoder.h"
#include "logger.h"
#include "common/codings/manchester.h"
#include "imgui/imgui.h"

// Return filesize
uint64_t getFilesize(std::string filepath);

#define BUFFER_SIZE 8192

namespace meteor
{
    METEORHRPTDecoderModule::METEORHRPTDecoderModule(std::string input_file, std::string output_file_hint, nlohmann::json parameters) 
        : ProcessingModule(input_file, output_file_hint, parameters),
          constellation(1.0, 0.15, demod_constellation_size),
          // ★ 初始化 Deframer 和 Reader
          bism_reader(-1),
          msumr_deframer(0x0218a7a392dd9abf, 64, 11850 * 8, 10, false),
          mtvza_deframer(0xFB386A45, 32, 248 * 8, 0, false),
          mtvza_deframer2(0x38fb456a, 32, 248 * 8, 0, false),
          bism_deframer(0x71DE2CD8, 32, 88 * 8, 0, false)
    {
        mtvza_reader2.endian_mode = true; // MTVZA2 必须设置大端模式
        def = std::make_shared<CADUDeframer>();
        soft_buffer = new int8_t[BUFFER_SIZE];
    }

    std::vector<ModuleDataType> METEORHRPTDecoderModule::getInputTypes()
    {
        return {DATA_FILE, DATA_STREAM};
    }

    std::vector<ModuleDataType> METEORHRPTDecoderModule::getOutputTypes()
    {
        return {DATA_FILE};
    }

    METEORHRPTDecoderModule::~METEORHRPTDecoderModule()
    {
        delete[] soft_buffer;
    }

    void METEORHRPTDecoderModule::process()
    {
        if (input_data_type == DATA_FILE)
            filesize = getFilesize(d_input_file);
        else
            filesize = 0;
        if (input_data_type == DATA_FILE)
            data_in = std::ifstream(d_input_file, std::ios::binary);
            
        data_out = std::ofstream(d_output_file_hint + ".cadu", std::ios::binary);
        d_output_files.push_back(d_output_file_hint + ".cadu");

        // ★ 新增：打开 BIS-M 的存储文件
        std::string out_dir = d_output_file_hint;
        size_t slash = out_dir.rfind('/');
        if (slash != std::string::npos)
            out_dir = out_dir.substr(0, slash);
        else
            out_dir = ".";
            
        bism_raw_out = std::ofstream(out_dir + "/BIS-M.raw", std::ios::binary);
        bism_bin_out = std::ofstream(out_dir + "/BIS-M.bin", std::ios::binary);

        logger->info("Using input data " + d_input_file);
        logger->info("Decoding to " + d_output_file_hint + ".cadu");

        time_t lastTime = 0;
        int frame_count = 0;

        // ★ 提取仪器数据的通用 Lambda
        auto process_instrument_data = [this](uint8_t *cadu) {
            std::vector<std::vector<uint8_t>> msumr_frames, mtvza_frames, mtvza_frames2, bism_frames;

            // 1. 解析 MSU-MR
            std::vector<uint8_t> msu_mr_data;
            msu_mr_data.insert(msu_mr_data.end(), &cadu[23 - 1], &cadu[23 - 1] + 238);
            msu_mr_data.insert(msu_mr_data.end(), &cadu[279 - 1], &cadu[279 - 1] + 238);
            msu_mr_data.insert(msu_mr_data.end(), &cadu[535 - 1], &cadu[535 - 1] + 238);
            msu_mr_data.insert(msu_mr_data.end(), &cadu[791 - 1], &cadu[791 - 1] + 234);
            msumr_frames = msumr_deframer.work(msu_mr_data.data(), msu_mr_data.size());
            for (auto &frame : msumr_frames) msumr_reader.work(frame.data());

            // 2. 解析 MTVZA
            std::vector<uint8_t> mtvza_data;
            mtvza_data.insert(mtvza_data.end(), &cadu[15 - 1], &cadu[15 - 1] + 8);
            mtvza_data.insert(mtvza_data.end(), &cadu[271 - 1], &cadu[271 - 1] + 8);
            mtvza_data.insert(mtvza_data.end(), &cadu[527 - 1], &cadu[527 - 1] + 8);
            mtvza_data.insert(mtvza_data.end(), &cadu[783 - 1], &cadu[783 - 1] + 8);
            mtvza_frames = mtvza_deframer.work(mtvza_data.data(), mtvza_data.size());
            mtvza_frames2 = mtvza_deframer2.work(mtvza_data.data(), mtvza_data.size());
            for (auto &frame : mtvza_frames) mtvza_reader.work(frame.data());
            for (auto &frame : mtvza_frames2) mtvza_reader2.work(frame.data());

            // 3. 解析 BIS-M 并存储
            std::vector<uint8_t> bism_data;
            bism_data.insert(bism_data.end(), &cadu[7 - 1], &cadu[7 - 1] + 4);
            bism_data.insert(bism_data.end(), &cadu[263 - 1], &cadu[263 - 1] + 4);
            bism_data.insert(bism_data.end(), &cadu[519 - 1], &cadu[519 - 1] + 4);
            bism_data.insert(bism_data.end(), &cadu[775 - 1], &cadu[775 - 1] + 4);

            if (bism_raw_out.is_open()) 
                bism_raw_out.write((char *)bism_data.data(), bism_data.size());

            bism_frames = bism_deframer.work(bism_data.data(), bism_data.size());
            for (auto &frame : bism_frames) {
                if (bism_bin_out.is_open()) 
                    bism_bin_out.write((char *)frame.data(), frame.size());
                bism_reader.work(frame.data());
            }
        };

        while (input_data_type == DATA_FILE ? !data_in.eof() : input_active.load())
        {
            // Read a buffer
            if (input_data_type == DATA_FILE)
                data_in.read((char *)soft_buffer, BUFFER_SIZE);
            else
                input_fifo->read((uint8_t *)soft_buffer, BUFFER_SIZE);

            std::vector<std::array<uint8_t, CADU_SIZE>> frames = def->work(soft_buffer, BUFFER_SIZE);

            // Count frames
            frame_count += frames.size();

            // Write to file
            if (frames.size() > 0)
            {
                for (std::array<uint8_t, CADU_SIZE> frm : frames)
                {
                    // ★ 新增：统计仪器行数
                    process_instrument_data(frm.data());

                    data_out.write((char *)&frm[0], CADU_SIZE);
                }
            }

            if (input_data_type == DATA_FILE)
                progress = data_in.tellg();

            module_stats["deframer_lock"] = def->getState() == 12;

            if (time(NULL) % 10 == 0 && lastTime != time(NULL))
            {
                lastTime = time(NULL);
                std::string deframer_state = def->getState() == 0 ? "NOSYNC" : (def->getState() == 2 || def->getState() == 6 ? "SYNCING" : "SYNCED");
                logger->info("Progress " + std::to_string(round(((double)progress / (double)filesize) * 1000.0) / 10.0) + "%%, Deframer : " + deframer_state + ", Frames : " + std::to_string(frame_count));
            }
        }

        logger->info("Decoding finished");

        data_out.close();
        // ★ 关闭 BIS-M 文件
        if (bism_raw_out.is_open()) bism_raw_out.close();
        if (bism_bin_out.is_open()) bism_bin_out.close();

        if (input_data_type == DATA_FILE)
            data_in.close();
    }

    void METEORHRPTDecoderModule::drawUI(bool window)
    {
        ImGui::Begin("METEOR HRPT Decoder", NULL, window ? 0 : NOWINDOW_FLAGS);

        // Constellation
        ImGui::BeginGroup();
        constellation.pushSofttAndGaussian(soft_buffer, 127, BUFFER_SIZE);
        constellation.draw();
        ImGui::EndGroup();

        ImGui::SameLine();

        ImGui::BeginGroup();
        {
            ImGui::Button("Deframer", {200 * ui_scale, 20 * ui_scale});
            {
                ImGui::Text("State : ");

                ImGui::SameLine();

                if (def->getState() == 0)
                    ImGui::TextColored(style::theme.red, "NOSYNC");
                else if (def->getState() == 2 || def->getState() == 6)
                    ImGui::TextColored(style::theme.orange, "SYNCING");
                else
                    ImGui::TextColored(style::theme.green, "SYNCED");
            }
        }
        ImGui::EndGroup();

        if (!streamingInput)
            ImGui::ProgressBar((double)progress / (double)filesize, ImVec2(ImGui::GetContentRegionAvail().x, 20 * ui_scale));

        // ★ 新增：底部实时预览行数
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::TextColored(style::theme.green, "MSU-MR: %d", msumr_reader.lines);
        ImGui::SameLine();
        ImGui::TextColored(style::theme.orange, " |  MTVZA: %d", std::max(mtvza_reader.lines, mtvza_reader2.lines));
        ImGui::SameLine();
        ImGui::TextColored(style::theme.blue, " |  BIS-M: %d", bism_reader.get_lines());

        ImGui::End();
    }

    std::string METEORHRPTDecoderModule::getID()
    {
        return "meteor_hrpt_decoder";
    }

    std::vector<std::string> METEORHRPTDecoderModule::getParameters()
    {
        return {"samplerate", "buffer_size", "baseband_format"};
    }

    std::shared_ptr<ProcessingModule> METEORHRPTDecoderModule::getInstance(std::string input_file, std::string output_file_hint, nlohmann::json parameters)
    {
        return std::make_shared<METEORHRPTDecoderModule>(input_file, output_file_hint, parameters);
    }
} // namespace meteor