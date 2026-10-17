#include "module_metop_ahrpt_decoder.h"
#include "common/codings/reedsolomon/reedsolomon.h"
#include "logger.h"
#include "common/widgets/themed_widgets.h"
#include "common/codings/randomization.h"

// 需要引入解析和计数的头文件
#include "common/ccsds/ccsds_aos/vcdu.h"
#include "common/ccsds/ccsds_aos/demuxer.h"
#include "instruments/avhrr/avhrr_metop_reader.h"
#include "instruments/iasi/iasi_imaging_reader.h"

#define BUFFER_SIZE 8192 * 2

// Return filesize
uint64_t getFilesize(std::string filepath);

namespace metop
{
    MetOpAHRPTDecoderModule::MetOpAHRPTDecoderModule(std::string input_file, std::string output_file_hint, nlohmann::json parameters) 
        : ProcessingModule(input_file, output_file_hint, parameters),
          d_viterbi_outsync_after(parameters["viterbi_outsync_after"].get<int>()),
          d_viterbi_ber_thresold(parameters["viterbi_ber_thresold"].get<float>()),
          viterbi(d_viterbi_ber_thresold, d_viterbi_outsync_after, BUFFER_SIZE),
          avhrr_reader(false, -1),      // 初始化 AVHRR reader
          demuxer_vcid9(882, true),     // 初始化 Demuxer
          demuxer_vcid10(882, true)     // 初始化 Demuxer
    {
        viterbi_out = new uint8_t[BUFFER_SIZE * 2];
        soft_buffer = new int8_t[BUFFER_SIZE];
        deframer.STATE_SYNCED = 18;
    }

    std::vector<ModuleDataType> MetOpAHRPTDecoderModule::getInputTypes()
    {
        return {DATA_FILE, DATA_STREAM};
    }

    std::vector<ModuleDataType> MetOpAHRPTDecoderModule::getOutputTypes()
    {
        return {DATA_FILE}; // 不再需要 STREAM，因为我们自己做完了
    }

    MetOpAHRPTDecoderModule::~MetOpAHRPTDecoderModule()
    {
        delete[] viterbi_out;
        delete[] soft_buffer;
    }

    void MetOpAHRPTDecoderModule::process()
    {
        if (input_data_type == DATA_FILE)
            filesize = getFilesize(d_input_file);
        else
            filesize = 0;
            
        if (input_data_type == DATA_FILE)
            data_in = std::ifstream(d_input_file, std::ios::binary);

        // ★ 修改：用固定名保存 CADU，防止与上游冲突
        std::string out_dir = d_output_file_hint;
        size_t slash = out_dir.rfind('/');
        if (slash != std::string::npos)
            out_dir = out_dir.substr(0, slash);
        else
            out_dir = ".";

        std::string cadu_path = out_dir + "/metop_ahrpt_raw.cadu";
        data_out = std::ofstream(cadu_path, std::ios::binary);
        d_output_files.push_back(cadu_path);

        logger->info("Using input symbols " + d_input_file);
        logger->info("Decoding to " + cadu_path);

        time_t lastTime = 0;

        reedsolomon::ReedSolomon rs(reedsolomon::RS223);

        uint8_t frame_buffer[1024 * 10];

        int noSyncsRuns = 0;

        while (input_data_type == DATA_FILE ? !data_in.eof() : input_active.load())
        {
            // Read a buffer
            if (input_data_type == DATA_FILE)
                data_in.read((char *)soft_buffer, BUFFER_SIZE);
            else
                input_fifo->read((uint8_t *)soft_buffer, BUFFER_SIZE);

            // Perform Viterbi decoding
            int num_samp = viterbi.work(soft_buffer, BUFFER_SIZE, viterbi_out);

            // Reconstruct into bytes and write to output file
            if (num_samp > 0)
            {
                // Deframe / derand
                int frames = deframer.work(viterbi_out, num_samp, frame_buffer);

                // Rare case where the viterbi might stay locked on
                // a bad phase at high SNR starting in the middle of a baseband
                // If it stays that way a bit too long, reset.
                if (deframer.getState() == deframer.STATE_NOSYNC)
                {
                    noSyncsRuns++;

                    if (noSyncsRuns >= 10)
                    {
                        viterbi.reset();
                        noSyncsRuns = 0;
                    }
                }
                else
                {
                    noSyncsRuns = 0;
                }

                for (int i = 0; i < frames; i++)
                {
                    uint8_t *cadu = &frame_buffer[i * 1024];

                    derand_ccsds(&cadu[4], 1024 - 4);

                    // RS Correction
                    rs.decode_interlaved(&cadu[4], true, 4, errors);

                    // Write it out
                    data_out.write((char *)cadu, 1024);

                    // ★ 新增：解析 VCDU，只对 AVHRR/IASI-IMG 计数
                    ccsds::ccsds_aos::VCDU vcdu = ccsds::ccsds_aos::parseVCDU(cadu);

                    if (vcdu.vcid == 9) // AVHRR/3
                    {
                        std::vector<ccsds::CCSDSPacket> ccsdsFrames = demuxer_vcid9.work(cadu);
                        for (ccsds::CCSDSPacket &pkt : ccsdsFrames)
                            if (pkt.header.apid == 103 || pkt.header.apid == 104)
                                avhrr_reader.work_metop(pkt);
                    }
                    else if (vcdu.vcid == 10) // IASI
                    {
                        std::vector<ccsds::CCSDSPacket> ccsdsFrames = demuxer_vcid10.work(cadu);
                        for (ccsds::CCSDSPacket &pkt : ccsdsFrames)
                            if (pkt.header.apid == 150) // IASI-IMG
                                iasi_reader_img.work(pkt);
                    }
                }
            }

            if (input_data_type == DATA_FILE)
                progress = data_in.tellg();

            // Update module stats
            module_stats["deframer_lock"] = deframer.getState() == deframer.STATE_SYNCED;
            module_stats["viterbi_ber"] = viterbi.ber();
            module_stats["viterbi_lock"] = viterbi.getState();
            module_stats["rs_avg"] = (errors[0] + errors[1] + errors[2] + errors[3]) / 4;
            
            // ★ 新增：更新计数统计
            module_stats["avhrr_lines"]    = avhrr_reader.lines;
            module_stats["iasi_img_lines"] = iasi_reader_img.lines * 64;

            if (time(NULL) % 10 == 0 && lastTime != time(NULL))
            {
                lastTime = time(NULL);
                std::string viterbi_state = viterbi.getState() == 0 ? "NOSYNC" : "SYNCED";
                std::string deframer_state = deframer.getState() == deframer.STATE_NOSYNC ? "NOSYNC" : (deframer.getState() == deframer.STATE_SYNCING ? "SYNCING" : "SYNCED");
                logger->info("Progress " + std::to_string(round(((double)progress / (double)filesize) * 1000.0) / 10.0) + "%%, Viterbi : " + viterbi_state + " BER : " + std::to_string(viterbi.ber()) + ", Deframer : " + deframer_state + 
                             ", AVHRR : " + std::to_string(avhrr_reader.lines) + " lines" + 
                             ", IASI-IMG : " + std::to_string(iasi_reader_img.lines * 64) + " lines");
            }
        }

        data_out.close();
        if (input_data_type == DATA_FILE)
            data_in.close();
            
        logger->info("AVHRR lines    : " + std::to_string(avhrr_reader.lines));
        logger->info("IASI-IMG lines : " + std::to_string(iasi_reader_img.lines * 64));
    }

    void MetOpAHRPTDecoderModule::drawUI(bool window)
    {
        ImGui::Begin("MetOp AHRPT Decoder", NULL, window ? 0 : NOWINDOW_FLAGS);

        float ber = viterbi.ber();

        ImGui::BeginGroup();
        {
            // Constellation
            {
                ImDrawList *draw_list = ImGui::GetWindowDrawList();
                ImVec2 rect_min = ImGui::GetCursorScreenPos();
                ImVec2 rect_max = { rect_min.x + 200 * ui_scale, rect_min.y + 200 * ui_scale };
                draw_list->AddRectFilled(rect_min, rect_max, style::theme.widget_bg);
                draw_list->PushClipRect(rect_min, rect_max);

                for (int i = 0; i < 2048; i++)
                {
                    draw_list->AddCircleFilled(ImVec2(ImGui::GetCursorScreenPos().x + (int)(100 * ui_scale + (((int8_t *)soft_buffer)[i * 2 + 0] / 127.0) * 100 * ui_scale) % int(200 * ui_scale),
                                                      ImGui::GetCursorScreenPos().y + (int)(100 * ui_scale + (((int8_t *)soft_buffer)[i * 2 + 1] / 127.0) * 100 * ui_scale) % int(200 * ui_scale)),
                                               2 * ui_scale,
                                               style::theme.constellation);
                }

                draw_list->PopClipRect();
                ImGui::Dummy(ImVec2(200 * ui_scale + 3, 200 * ui_scale + 3));
            }
        }
        ImGui::EndGroup();

        ImGui::SameLine();

        ImGui::BeginGroup();
        {
            ImGui::Button("Viterbi", {200 * ui_scale, 20 * ui_scale});
            {
                ImGui::Text("State : ");

                ImGui::SameLine();

                if (viterbi.getState() == 0)
                    ImGui::TextColored(style::theme.red, "NOSYNC");
                else
                    ImGui::TextColored(style::theme.green, "SYNCED");

                ImGui::Text("BER   : ");
                ImGui::SameLine();
                ImGui::TextColored(viterbi.getState() == 0 ? style::theme.red : style::theme.green, UITO_C_STR(ber));

                std::memmove(&ber_history[0], &ber_history[1], (200 - 1) * sizeof(float));
                ber_history[200 - 1] = ber;

                widgets::ThemedPlotLines(style::theme.plot_bg.Value, "", ber_history, IM_ARRAYSIZE(ber_history), 0, "", 0.0f, 1.0f,
                                         ImVec2(200 * ui_scale, 50 * ui_scale));
            }

            ImGui::Spacing();

            ImGui::Button("Deframer", {200 * ui_scale, 20 * ui_scale});
            {
                ImGui::Text("State : ");

                ImGui::SameLine();

                if (viterbi.getState() == 0)
                {
                    ImGui::TextColored(ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled), "NOSYNC");
                }
                else
                {
                    if (deframer.getState() == deframer.STATE_NOSYNC)
                        ImGui::TextColored(style::theme.red, "NOSYNC");
                    else if (deframer.getState() == deframer.STATE_SYNCING)
                        ImGui::TextColored(style::theme.orange, "SYNCING");
                    else
                        ImGui::TextColored(style::theme.green, "SYNCED");
                }
            }

            ImGui::Spacing();

            ImGui::Button("Reed-Solomon", {200 * ui_scale, 20 * ui_scale});
            {
                ImGui::Text("RS    : ");
                for (int i = 0; i < 4; i++)
                {
                    ImGui::SameLine();

                    if (viterbi.getState() == 0 || deframer.getState() == deframer.STATE_NOSYNC)
                    {
                        ImGui::TextColored(ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled), "%i ", i);
                    }
                    else
                    {
                        if (errors[i] == -1)
                            ImGui::TextColored(style::theme.red, "%i ", i);
                        else if (errors[i] > 0)
                            ImGui::TextColored(style::theme.orange, "%i ", i);
                        else
                            ImGui::TextColored(style::theme.green, "%i ", i);
                    }
                }
            }

            ImGui::Spacing();

            // ★ 新增：Counters UI 显示
            ImGui::Button("Counters", {200 * ui_scale, 20 * ui_scale});
            {
                ImGui::Text("AVHRR    : ");
                ImGui::SameLine();
                ImGui::TextColored(style::theme.green, "%d", avhrr_reader.lines);
                
                ImGui::Text("IASI-IMG : ");
                ImGui::SameLine();
                ImGui::TextColored(style::theme.green, "%d", iasi_reader_img.lines * 64);
            }
        }
        ImGui::EndGroup();

        if (!streamingInput)
            ImGui::ProgressBar((double)progress / (double)filesize, ImVec2(ImGui::GetContentRegionAvail().x, 20 * ui_scale));

        ImGui::End();
    }

    std::string MetOpAHRPTDecoderModule::getID()
    {
        return "metop_ahrpt_decoder";
    }

    std::vector<std::string> MetOpAHRPTDecoderModule::getParameters()
    {
        return {"viterbi_outsync_after", "viterbi_ber_thresold"};
    }

    std::shared_ptr<ProcessingModule> MetOpAHRPTDecoderModule::getInstance(std::string input_file, std::string output_file_hint, nlohmann::json parameters)
    {
        return std::make_shared<MetOpAHRPTDecoderModule>(input_file, output_file_hint, parameters);
    }
} // namespace metop