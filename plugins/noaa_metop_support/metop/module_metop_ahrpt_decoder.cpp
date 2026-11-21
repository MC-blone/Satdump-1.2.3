#include "module_metop_ahrpt_decoder.h"
#include "common/codings/reedsolomon/reedsolomon.h"
#include "logger.h"
#include "common/widgets/themed_widgets.h"
#include "common/codings/randomization.h"

// 需要引入解析和计数的头文件
#include "common/ccsds/ccsds_aos/vcdu.h"
#include "common/ccsds/ccsds_aos/demuxer.h"
#include "../instruments/avhrr/avhrr_reader.h"
#include "instruments/iasi/iasi_imaging_reader.h"

#include <cstring>
#include <ctime>
#include <vector>

#define BUFFER_SIZE 8192 * 2

// Return filesize
uint64_t getFilesize(std::string filepath);

namespace metop
{
    MetOpAHRPTDecoderModule::MetOpAHRPTDecoderModule(std::string input_file, std::string output_file_hint, nlohmann::json parameters)
        : ProcessingModule(input_file, output_file_hint, parameters),
          d_viterbi_outsync_after(parameters["viterbi_outsync_after"].get<int>()),
          d_viterbi_ber_thresold(parameters["viterbi_ber_thresold"].get<float>()),
          d_cadu_size(parameters.contains("cadu_size") ? parameters["cadu_size"].get<int>() : 1024),  // ★ 默认 1024
          viterbi(d_viterbi_ber_thresold, d_viterbi_outsync_after, BUFFER_SIZE),
          avhrr_reader(false, -1),
          demuxer_vcid9(882, true),
          demuxer_vcid10(882, true)
    {
        viterbi_out = new uint8_t[BUFFER_SIZE * 2];
        soft_buffer = new int8_t[BUFFER_SIZE];
        deframer.STATE_SYNCED = 18;

        // ★ Fix 开关(来自 parameters 顶层,由 SatDump 注入)
        use_fix = d_parameters.contains("Use Fix") ? d_parameters["Use Fix"].get<bool>() : false;
    }

    std::vector<ModuleDataType> MetOpAHRPTDecoderModule::getInputTypes()
    {
        return {DATA_FILE, DATA_STREAM};
    }

    std::vector<ModuleDataType> MetOpAHRPTDecoderModule::getOutputTypes()
    {
        return {DATA_FILE};
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
        logger->info("CADU size: " + std::to_string(d_cadu_size));

        time_t lastTime = 0;

        reedsolomon::ReedSolomon rs(reedsolomon::RS223);

        // ★ 使用 d_cadu_size 分配帧缓冲
        std::vector<uint8_t> frame_buffer(d_cadu_size * 10);

        int noSyncsRuns = 0;

        while (input_data_type == DATA_FILE ? !data_in.eof() : input_active.load())
        {
            if (input_data_type == DATA_FILE)
                data_in.read((char *)soft_buffer, BUFFER_SIZE);
            else
                input_fifo->read((uint8_t *)soft_buffer, BUFFER_SIZE);

            int num_samp = viterbi.work(soft_buffer, BUFFER_SIZE, viterbi_out);

            if (num_samp > 0)
            {
                // ============ 原解码路径(零改动) ============
                int frames = deframer.work(viterbi_out, num_samp, frame_buffer.data());

                // ============ Fix 状态跟踪 ============
                int def_state = deframer.getState();
                int vit_state = viterbi.getState();

                // 1) 正常同步 → 清除修复状态,回到 SYNCED
                if (def_state == deframer.STATE_SYNCED && vit_state != 0)
                {
                    consecutive_syncs++;
                    last_sync_time  = time(NULL);
                    fixing_active   = false;
                }

                // 2) 超过 120s 没有重新同步,彻底失效
                if (time(NULL) - last_sync_time >= 120)
                {
                    consecutive_syncs = 0;
                    fixing_active     = false;
                }

                bool can_fix = use_fix
                            && (consecutive_syncs >= 2)
                            && (time(NULL) - last_sync_time < 120);

                // 3) 条件不再成立 → 退出修复
                if (!can_fix)
                    fixing_active = false;

                // 4) 只在 Deframer NOSYNC 且本帧 0 帧时尝试修复
                if (can_fix && def_state == deframer.STATE_NOSYNC
                            && frames == 0 && num_samp >= 8192)
                {
                    int num_bytes  = num_samp / 8;
                    int max_search = num_bytes - d_cadu_size;
                    for (int k = 0; k <= max_search; k++)
                    {
                        if (viterbi_out[k]     == 0x1A &&
                            viterbi_out[k + 1] == 0xCF &&
                            viterbi_out[k + 2] == 0xFC &&
                            viterbi_out[k + 3] == 0x1D)
                        {
                            std::memcpy(frame_buffer.data(), &viterbi_out[k], d_cadu_size);
                            frames        = 1;
                            fixing_active = true;   // ← SYNCED(FIX)
                            break;
                        }
                    }
                }
                // ======================================

                // 原逻辑:强制 reset Viterbi
                if (deframer.getState() == deframer.STATE_NOSYNC)
                {
                    noSyncsRuns++;
                    if (noSyncsRuns >= 10) { viterbi.reset(); noSyncsRuns = 0; }
                }
                else
                {
                    noSyncsRuns = 0;
                }

                for (int i = 0; i < frames; i++)
                {
                    uint8_t *cadu = &frame_buffer[i * d_cadu_size];

                    derand_ccsds(&cadu[4], d_cadu_size - 4);
                    rs.decode_interlaved(&cadu[4], true, 4, errors);
                    data_out.write((char *)cadu, d_cadu_size);

                    ccsds::ccsds_aos::VCDU vcdu = ccsds::ccsds_aos::parseVCDU(cadu);

                    if (vcdu.vcid == 9)
                    {
                        std::vector<ccsds::CCSDSPacket> ccsdsFrames = demuxer_vcid9.work(cadu);
                        for (ccsds::CCSDSPacket &pkt : ccsdsFrames)
                            if (pkt.header.apid == 103 || pkt.header.apid == 104)
                                avhrr_reader.work_metop(pkt);
                    }
                    else if (vcdu.vcid == 10)
                    {
                        std::vector<ccsds::CCSDSPacket> ccsdsFrames = demuxer_vcid10.work(cadu);
                        for (ccsds::CCSDSPacket &pkt : ccsdsFrames)
                            if (pkt.header.apid == 150)
                                iasi_reader_img.work(pkt);
                    }
                }
            }

            if (input_data_type == DATA_FILE)
                progress = data_in.tellg();

            module_stats["deframer_lock"] = deframer.getState() == deframer.STATE_SYNCED;
            module_stats["viterbi_ber"]    = viterbi.ber();
            module_stats["viterbi_lock"]   = viterbi.getState();
            module_stats["rs_avg"]         = (errors[0] + errors[1] + errors[2] + errors[3]) / 4;
            module_stats["avhrr_lines"]    = avhrr_reader.lines;
            module_stats["iasi_img_lines"] = iasi_reader_img.lines * 64;

            if (time(NULL) % 10 == 0 && lastTime != time(NULL))
            {
                lastTime = time(NULL);
                std::string viterbi_state = viterbi.getState() == 0 ? "NOSYNC" : "SYNCED";

                std::string deframer_state;
                if (viterbi.getState() == 0)
                    deframer_state = "NOSYNC";
                else if (fixing_active)
                    deframer_state = "SYNCED(FIX)";
                else if (deframer.getState() == deframer.STATE_NOSYNC)
                    deframer_state = "NOSYNC";
                else if (deframer.getState() == deframer.STATE_SYNCING)
                    deframer_state = "SYNCING";
                else
                    deframer_state = "SYNCED";

                logger->info("Progress " + std::to_string(round(((double)progress / (double)filesize) * 1000.0) / 10.0) + "%, Viterbi : " + viterbi_state + " BER : " + std::to_string(viterbi.ber()) + ", Deframer : " + deframer_state +
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

            // ★ Deframer: NOSYNC → SYNCED(FIX) → SYNCED
            ImGui::Button("Deframer", {200 * ui_scale, 20 * ui_scale});
            {
                ImGui::Text("State : ");
                ImGui::SameLine();

                if (viterbi.getState() == 0)
                {
                    ImGui::TextColored(ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled), "NOSYNC");
                }
                else if (fixing_active)
                {
                    ImGui::TextColored(style::theme.green, "SYNCED(FIX)");
                }
                else if (deframer.getState() == deframer.STATE_NOSYNC)
                {
                    ImGui::TextColored(style::theme.red, "NOSYNC");
                }
                else if (deframer.getState() == deframer.STATE_SYNCING)
                {
                    ImGui::TextColored(style::theme.orange, "SYNCING");
                }
                else
                {
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

            // ★ FIX 汇总
            ImGui::Button("FIX", {200 * ui_scale, 20 * ui_scale});
            {
                ImGui::Text("Status : ");
                ImGui::SameLine();
                if (!use_fix)
                    ImGui::TextColored(style::theme.red, "Disabled");
                else if (fixing_active)
                    ImGui::TextColored(style::theme.green, "Use Fix");
                else
                    ImGui::TextColored(style::theme.orange, "Enabled (idle)");

                ImGui::Text("Syncs  : ");
                ImGui::SameLine();
                ImGui::TextColored(style::theme.blue, "%d", consecutive_syncs);
            }

            ImGui::Spacing();

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
        // ★ cadu_size 作为参数暴露(SatDump 自动生成输入框,默认 1024)
        return {"viterbi_outsync_after", "viterbi_ber_thresold", "Use Fix", "cadu_size"};
    }

    std::shared_ptr<ProcessingModule> MetOpAHRPTDecoderModule::getInstance(std::string input_file, std::string output_file_hint, nlohmann::json parameters)
    {
        return std::make_shared<MetOpAHRPTDecoderModule>(input_file, output_file_hint, parameters);
    }
} // namespace metop