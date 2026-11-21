#include "module_metop_ahrpt_decoder.h"
#include "common/codings/reedsolomon/reedsolomon.h"
#include "logger.h"
#include "common/widgets/themed_widgets.h"
#include "common/codings/randomization.h"

#include "common/ccsds/ccsds_aos/vcdu.h"
#include "common/ccsds/ccsds_aos/demuxer.h"
#include "../instruments/avhrr/avhrr_reader.h"
#include "instruments/iasi/iasi_imaging_reader.h"

#include <cstring>
#include <ctime>
#include <vector>

#define BUFFER_SIZE 8192 * 2

// CCSDS 同步字
static const uint32_t CCSDS_SYNC_WORD = 0x1ACFFC1D;

// Return filesize
uint64_t getFilesize(std::string filepath);

namespace metop
{
    MetOpAHRPTDecoderModule::MetOpAHRPTDecoderModule(std::string input_file, std::string output_file_hint, nlohmann::json parameters)
        : ProcessingModule(input_file, output_file_hint, parameters),
          d_viterbi_outsync_after(parameters["viterbi_outsync_after"].get<int>()),
          d_viterbi_ber_thresold(parameters["viterbi_ber_thresold"].get<float>()),
          d_cadu_size(parameters.contains("cadu_size") ? parameters["cadu_size"].get<int>() : 1024),
          viterbi(d_viterbi_ber_thresold, d_viterbi_outsync_after, BUFFER_SIZE),
          avhrr_reader(false, -1),
          demuxer_vcid9(882, true),
          demuxer_vcid10(882, true)
    {
        viterbi_out = new uint8_t[BUFFER_SIZE * 2];
        soft_buffer = new int8_t[BUFFER_SIZE];
        deframer.STATE_SYNCED = 18;

        // ★ 参数名统一为 use_fix（小写下划线）
        use_fix = d_parameters.contains("use_fix") ? d_parameters["use_fix"].get<bool>() : false;
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

    void MetOpAHRPTDecoderModule::init() {}

    // ★ 实时模式停止：关 fifo + 设 input_active
    void MetOpAHRPTDecoderModule::stop()
    {
        input_active = false;
        if (input_data_type != DATA_FILE && input_fifo)
            input_fifo->stopReader();
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
        logger->info("use_fix  : " + std::string(use_fix ? "true" : "false"));

        time_t lastTime = 0;

        reedsolomon::ReedSolomon rs(reedsolomon::RS223);

        std::vector<uint8_t> frame_buffer(d_cadu_size * 10);

        int noSyncsRuns = 0;

        while (input_data_type == DATA_FILE ? !data_in.eof() : input_active.load())
        {
            // ★ 实时模式：检查 input_active + fifo->read 返回值，保证能即时退出
            if (input_data_type == DATA_FILE)
            {
                data_in.read((char *)soft_buffer, BUFFER_SIZE);
            }
            else
            {
                if (!input_active.load()) break;
                if (input_fifo->read((uint8_t *)soft_buffer, BUFFER_SIZE) < 0) break;
                if (!input_active.load()) break;
            }

            // ================= Viterbi（用户原有修复保持不动） =================
            int num_samp = viterbi.work(soft_buffer, BUFFER_SIZE, viterbi_out);
            // num_samp 单位 = bits（BUFFER_SIZE * 3/4 = 12288，viterbi_out 有效 1536 字节）
            // ==================================================================

            if (num_samp > 0)
            {
                // ============ 原解码路径（零改动） ============
                int frames = deframer.work(viterbi_out, num_samp, frame_buffer.data());

                // ============ Fix 状态跟踪（Viterbi 之后、Deframer NOSYNC 时补偿） ============
                int def_state = deframer.getState();
                int vit_state = viterbi.getState();

                // 1) 正常同步 → 清除修复状态
                if (def_state == deframer.STATE_SYNCED && vit_state != 0)
                {
                    consecutive_syncs++;
                    last_sync_time = time(NULL);
                    fixing_active  = false;
                }

                // 2) 120s 没恢复 → 放弃修复
                if (time(NULL) - last_sync_time >= 120)
                {
                    consecutive_syncs = 0;
                    fixing_active     = false;
                }

                bool can_fix = use_fix
                            && (consecutive_syncs >= 2)
                            && (time(NULL) - last_sync_time < 120);

                if (!can_fix)
                    fixing_active = false;

                // 3) ★ 修复：仅在 Viterbi 仍锁、Deframer NOSYNC、本帧 0 帧时补偿
                if (can_fix && vit_state != 0
                            && def_state == deframer.STATE_NOSYNC
                            && frames == 0
                            && num_samp >= d_cadu_size * 8 + 32)   // bits
                {
                    int      total_bits = num_samp;                 // ← 单位就是 bits，不乘 8
                    int      cadu_bits  = d_cadu_size * 8;
                    uint32_t window     = 0;
                    int      found_bit  = -1;

                    for (int b = 0; b < total_bits; b++)
                    {
                        int bit = (viterbi_out[b >> 3] >> (7 - (b & 7))) & 1;
                        window  = (window << 1) | (uint32_t)bit;
                        if (b >= 31 && window == CCSDS_SYNC_WORD)
                        {
                            int start_bit = b - 31;
                            if (total_bits - start_bit >= cadu_bits)
                            {
                                found_bit = start_bit;
                                break;
                            }
                        }
                    }

                    if (found_bit >= 0)
                    {
                        std::memset(frame_buffer.data(), 0, d_cadu_size);
                        for (int i = 0; i < cadu_bits; i++)
                        {
                            int src = found_bit + i;
                            int bit = (viterbi_out[src >> 3] >> (7 - (src & 7))) & 1;
                            if (bit)
                                frame_buffer[i >> 3] |= (1 << (7 - (i & 7)));
                        }
                        frames        = 1;
                        fixing_active = true;
                    }
                }
                // ====================================================================

                // 原逻辑：10 轮 NOSYNC 后 reset Viterbi
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

            module_stats["deframer_lock"]  = deframer.getState() == deframer.STATE_SYNCED;
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
                if (viterbi.getState() == 0)                            deframer_state = "NOSYNC";
                else if (fixing_active)                                 deframer_state = "SYNCED(FIX)";
                else if (deframer.getState() == deframer.STATE_NOSYNC)  deframer_state = "NOSYNC";
                else if (deframer.getState() == deframer.STATE_SYNCING) deframer_state = "SYNCING";
                else                                                    deframer_state = "SYNCED";

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
        ImGui::EndGroup();

        ImGui::SameLine();

        ImGui::BeginGroup();
        {
            // Viterbi
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

            // Deframer
            ImGui::Button("Deframer", {200 * ui_scale, 20 * ui_scale});
            {
                ImGui::Text("State : ");
                ImGui::SameLine();

                if (viterbi.getState() == 0)
                    ImGui::TextColored(ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled), "NOSYNC");
                else if (fixing_active)
                    ImGui::TextColored(style::theme.green, "SYNCED(FIX)");
                else if (deframer.getState() == deframer.STATE_NOSYNC)
                    ImGui::TextColored(style::theme.red, "NOSYNC");
                else if (deframer.getState() == deframer.STATE_SYNCING)
                    ImGui::TextColored(style::theme.orange, "SYNCING");
                else
                    ImGui::TextColored(style::theme.green, "SYNCED");
            }

            ImGui::Spacing();

            // Reed-Solomon
            ImGui::Button("Reed-Solomon", {200 * ui_scale, 20 * ui_scale});
            {
                ImGui::Text("RS    : ");
                for (int i = 0; i < 4; i++)
                {
                    ImGui::SameLine();
                    if (viterbi.getState() == 0 || deframer.getState() == deframer.STATE_NOSYNC)
                        ImGui::TextColored(ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled), "%i ", i);
                    else
                    {
                        if (errors[i] == -1)      ImGui::TextColored(style::theme.red,    "%i ", i);
                        else if (errors[i] > 0)   ImGui::TextColored(style::theme.orange, "%i ", i);
                        else                      ImGui::TextColored(style::theme.green,  "%i ", i);
                    }
                }
            }

            ImGui::Spacing();
            ImGui::Checkbox("Show Fix Info", &show_fix_window);
        }
        ImGui::EndGroup();

        if (!streamingInput)
            ImGui::ProgressBar((double)progress / (double)filesize, ImVec2(ImGui::GetContentRegionAvail().x, 20 * ui_scale));

        ImGui::End();

        // ★ 浮动窗口
        if (show_fix_window)
        {
            if (ImGui::Begin("MetOp AHRPT Fix Info", &show_fix_window))
            {
                ImGui::Text("use_fix : ");
                ImGui::SameLine();
                if (use_fix) ImGui::TextColored(style::theme.green, "Enabled");
                else         ImGui::TextColored(style::theme.red,   "Disabled");

                ImGui::Text("State   : ");
                ImGui::SameLine();
                if (viterbi.getState() == 0)
                    ImGui::TextColored(ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled), "NOSYNC");
                else if (fixing_active)
                    ImGui::TextColored(style::theme.green, "SYNCED(FIX)");
                else if (deframer.getState() == deframer.STATE_NOSYNC)
                    ImGui::TextColored(style::theme.red, "NOSYNC");
                else if (deframer.getState() == deframer.STATE_SYNCING)
                    ImGui::TextColored(style::theme.orange, "SYNCING");
                else
                    ImGui::TextColored(style::theme.green, "SYNCED");

                ImGui::Text("Fixing  : ");
                ImGui::SameLine();
                if (!use_fix)
                    ImGui::TextColored(ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled), "-");
                else if (fixing_active)
                    ImGui::TextColored(style::theme.green, "ACTIVE");
                else
                    ImGui::TextColored(style::theme.orange, "IDLE");

                ImGui::Text("Syncs   : %d", consecutive_syncs);
                ImGui::Text("CADU sz : %d", d_cadu_size);

                ImGui::Separator();
                ImGui::TextColored(style::theme.green, "AVHRR    : %d lines", avhrr_reader.lines);
                ImGui::TextColored(style::theme.green, "IASI-IMG : %d lines", iasi_reader_img.lines * 64);
            }
            ImGui::End();
        }
    }

    std::string MetOpAHRPTDecoderModule::getID()
    {
        return "metop_ahrpt_decoder";
    }

    std::vector<std::string> MetOpAHRPTDecoderModule::getParameters()
    {
        return {"viterbi_outsync_after", "viterbi_ber_thresold", "use_fix", "cadu_size"};
    }

    std::shared_ptr<ProcessingModule> MetOpAHRPTDecoderModule::getInstance(std::string input_file, std::string output_file_hint, nlohmann::json parameters)
    {
        return std::make_shared<MetOpAHRPTDecoderModule>(input_file, output_file_hint, parameters);
    }
} // namespace metop