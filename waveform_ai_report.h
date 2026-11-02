#ifndef WAVEFORM_AI_REPORT_H
#define WAVEFORM_AI_REPORT_H

#include "waveform_quality.h"
#include "waveform_statistics.h"
#include "spectrum_analyzer.h"
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QMap>
#include <QSet>

// The model selects evidence-backed explanations. Measurement values and all
// displayed diagnostic statements remain local, including when the model fails.
namespace WaveformAi {
struct Evidence {
    QJsonObject facts;
    QMap<QString, QString> focus;
    QMap<QString, QString> checks;
    QString localReport;
};

inline QString number(double value) { return QString::number(value, 'g', 8); }

inline Evidence buildEvidence(const QVector<double> &data, double rate, double probe,
                              const QString &channel, const QStringList &quality, bool record,
                              const WaveformStatistics &stats, const FrequencyEstimate &frequency,
                              const SpectrumResult &spectrum)
{
    Evidence e;
    const bool validRate = rate > 0 && std::isfinite(rate);
    if (!(probe > 0) || !std::isfinite(probe)) probe = 1;
    const WaveformRailCheck rails = checkWaveformRails(data, probe);
    const double duration = validRate && data.size() > 1 ? (data.size() - 1) / rate : 0;
    const double cycles = frequency.valid ? duration * frequency.frequencyHz : 0;
    e.facts = QJsonObject{{"channel", channel}, {"source", record ? "complete_record" : "preview_snapshot"},
        {"sample_count", data.size()}, {"sample_rate_known", validRate},
        {"probe_factor_already_applied", probe}, {"finite_samples", rails.finite},
        {"adc_rail_points", rails.railPoints}, {"adc_rail_low_V", -5.0 + 10.0 / 4096.0},
        {"adc_rail_high_V", 5.0 - 20.0 / 4096.0}, {"quality", QJsonArray::fromStringList(quality)}};
    e.localReport = "### 测量结果（程序计算）\n\n" + channel + " · "
        + (record ? "完整记录" : "预览快照") + " · " + QString::number(data.size()) + " 点\n\n";
    if (validRate) {
        e.facts.insert("sample_rate_Hz", rate);
        e.facts.insert("duration_s", duration);
        e.localReport += "有效采样率 " + number(rate) + " Hz；首末点时间跨度 " + number(duration * 1000) + " ms。\n\n";
    } else e.localReport += "有效采样率未知，不能可靠换算频率和时间。\n\n";
    if (stats.valid) {
        e.facts.insert("statistics", QJsonObject{{"mean_V", stats.mean}, {"rms_V", stats.rms},
            {"min_V", stats.minimum}, {"max_V", stats.maximum}, {"vpp_V", stats.peakToPeak}});
        e.localReport += "| 峰峰值 | RMS | 平均值 | 最小值 | 最大值 |\n|---|---|---|---|---|\n| "
            + number(stats.peakToPeak) + " V | " + number(stats.rms) + " V | " + number(stats.mean)
            + " V | " + number(stats.minimum) + " V | " + number(stats.maximum) + " V |\n\n";
    }
    if (frequency.valid) {
        e.facts.insert("time_frequency_Hz", frequency.frequencyHz);
        e.facts.insert("estimated_cycle_count", cycles);
        e.localReport += "时域频率 " + number(frequency.frequencyHz) + " Hz；快照覆盖约 " + number(cycles) + " 个估计周期。\n\n";
        if (cycles >= 3)
            e.focus.insert("cycle_coverage", "当前快照覆盖至少 3 个估计周期，可用于本次波形分析；单次快照不能证明长期稳定。");
        else e.focus.insert("short_coverage", "快照少于 3 个估计周期，周期判断依据有限；建议扩大采集时间窗口。");
    } else e.focus.insert("frequency_unavailable", "本地未获得可靠的周期频率；直流、信号过弱、非周期信号或覆盖不足均可能造成这种结果，不能直接认定设备故障。");
    if (spectrum.valid) {
        const double peakPhase = spectrum.phaseDegrees.value(spectrum.peakBin);
        e.facts.insert("spectrum", QJsonObject{{"peak_Hz", spectrum.peakFrequencyHz}, {"peak_dBV_RMS", spectrum.peakDbv},
            {"median_floor_dBV", spectrum.noiseFloorDbv}, {"fft_size", spectrum.fftSize},
            {"bin_spacing_Hz", spectrum.resolutionHz}, {"second_peak_multiple_dBV", spectrum.secondHarmonicDbv},
            {"third_peak_multiple_dBV", spectrum.thirdHarmonicDbv}, {"peak_bin_phase_deg", peakPhase},
            {"phase_reference", "cosine, first point of the last FFT segment, wrapped to [-180,180] degrees"}});
        e.localReport += "FFT 主峰 " + number(spectrum.peakFrequencyHz) + " Hz；幅度 " + number(spectrum.peakDbv)
            + " dBV RMS；Hann 窗 " + QString::number(spectrum.fftSize) + " 点，格点间隔 " + number(spectrum.resolutionHz) + " Hz。\n\n";
        e.localReport += "中位噪声底近似 " + number(spectrum.noiseFloorDbv) + " dBV；主峰格点相位 " + number(peakPhase)
            + "°（FFT 段首点、余弦参考；与插值频率位置不同）。\n\n";
        e.focus.insert("phase_reference", "单路相位以本次 FFT 段起点为时间零点，使用余弦参考；改变段起点会改变相位，低幅度分量相位不可靠。这不是电路输入输出相位差。");
        e.focus.insert("fft_limits", "FFT 格点间隔不等于分辨邻近频率的能力。中位噪声底不是总噪声 RMS，本次不据此计算 SNR 或 THD；主峰的 2×/3×位置也不等于已确认谐波。");
        if (frequency.valid) {
            const bool agree = std::abs(frequency.frequencyHz - spectrum.peakFrequencyHz)
                <= std::max(2 * spectrum.resolutionHz, frequency.frequencyHz * 0.03);
            e.facts.insert("time_fft_frequency_agreement", agree);
            e.focus.insert(agree ? "frequency_agreement" : "frequency_difference", agree
                ? "时域估计与 FFT 主峰在本次比较容差内相符，支持当前主导频率判断；不能据此证明波形无失真。"
                : "时域估计与 FFT 主峰未在本次比较容差内相符。主峰可能不是基频，需结合完整波形和频谱检查。");
        }
    }
    e.localReport += "### 数据质量与适用范围（本地检查）\n\n";
    bool previewGap = false;
    QString dma = "DMA 状态未确认：快照没有设备溢出状态，不能据此确认有无溢出。";
    for (const QString &item : quality) {
        if (item.startsWith("DMA ")) dma = item;
        else if (!record && item.startsWith("数据不连续")) previewGap = true;
        else e.localReport += "- " + item + "\n";
    }
    e.facts.insert("dma_status_description", dma);
    e.facts.insert("preview_frames_not_adjacent", previewGap);
    e.localReport += "- " + dma + "\n";
    if (dma.startsWith("DMA 状态未确认")) e.focus.insert("dma_unknown", dma);
    else if (dma.startsWith("DMA 溢出")) e.focus.insert("dma_overflow_reported", dma);
    else e.focus.insert("dma_no_overflow_reported", "最近 type 3 未报告溢出；这是设备状态快照，不能作为历史连续或从未溢出的证明。");
    if (previewGap) {
        const QString text = "预览帧之间未衔接，程序没有跨空档拼接波形。板端定时取最新窗口时可产生这种间隔；这不等于单帧内部有断点，也不能据此认定 DMA 溢出或网络丢包。";
        e.localReport += "- " + text + "\n";
        e.focus.insert("preview_gap_scope", text);
    }
    if (data.isEmpty() || !rails.finite) {
        e.localReport += "- ADC 量程检查不完整：存在空数据或无效数值。\n";
    } else if (rails.railPoints > 0) {
        const QString text = QString("%1 个点触及 ADC 端量程边界阈值，存在饱和风险；需用原始码值和实际输入确认，不能直接确诊削顶。").arg(rails.railPoints);
        e.localReport += "- " + text + "\n";
        e.focus.insert("adc_rail_risk", text);
        e.checks.insert("reduce_input", "适当降低输入幅度，重复测量原始码值与波形；观察量程边界采样点是否减少，并核对 ADC 前端量程。");
    } else {
        const QString text = "当前有限采样点未触及 ADC 端量程边界阈值。正负峰值相等或落在量化台阶上都不是削顶证据；这项检查不能排除其他电路的限幅或失真。";
        e.localReport += "- " + text + "\n";
        e.focus.insert("adc_within_range", text);
    }
    if (stats.valid && frequency.valid && cycles >= 10 && rails.finite && rails.railPoints == 0 && stats.peakToPeak > 0) {
        const double acRms = std::sqrt(std::max(0.0, stats.rms * stats.rms - stats.mean * stats.mean));
        const double expected = stats.peakToPeak / (2 * std::sqrt(2.0));
        if (std::abs(acRms - expected) <= expected * 0.05) {
            e.facts.insert("sine_amplitude_relation_within_5_percent", true);
            e.focus.insert("sine_amplitude_relation", "交流 RMS 与峰峰值满足接近正弦的幅值关系（相对差不超过 5%）；这只是辅助依据，不能证明信号是纯正弦或排除失真。");
        }
    }
    e.localReport += "- 电压已应用探头倍率 ×" + number(probe) + "；ADC 边界检查使用换算前电压。单次快照不能证明长期趋势或稳定性。\n\n";
    e.checks.insert("repeat_measurement", "保持接线和设置不变，再抓取几次快照，比较频率、Vpp 和 RMS；需要判断长期稳定时应记录多次测量。");
    e.checks.insert("verify_probe", "核对 ADC 通道、探头倍率和外部衰减设置；与示波器比较时使用相同接地点和电压倍率。");
    e.checks.insert("inspect_waveform", "在原始波形中放大几个完整周期，并与外部示波器对照；判断平顶或畸变应查看完整采样，而不是发送给 AI 的抽取点。");
    if (!record || cycles < 3) e.checks.insert("capture_record", "需要更长的连续数据时，调整采集窗口后获取完整记录；不要把有空档的预览帧当成连续记录拼接。");
    if (spectrum.valid) e.checks.insert("inspect_spectrum", "结合完整频谱检查主峰和其他分量；要验证谐波或失真，需要控制采样条件并进行相应数值计算，不能只用抽取形状或噪声底差值判断。");
    return e;
}

inline QJsonArray entries(const QMap<QString, QString> &items)
{
    QJsonArray result;
    for (auto it = items.cbegin(); it != items.cend(); ++it)
        result.append(QJsonObject{{"id", it.key()}, {"meaning", it.value()}});
    return result;
}

inline QString requestInput(const Evidence &e, const QVector<double> &data, bool spectrumMode)
{
    QJsonArray samples;
    const int count = std::min(512, data.size());
    for (int i = 0; i < count; ++i) {
        const int index = count <= 1 ? 0 : int(qint64(i) * (data.size() - 1) / (count - 1));
        // Original indices are explicit: floor-rounded selection is not uniformly
        // spaced at 1/fs and must never be fed back into an FFT as adjacent data.
        samples.append(QJsonObject{{"original_index", index},
            {"voltage_V", std::isfinite(data[index]) ? QJsonValue(data[index]) : QJsonValue()}});
    }
    QJsonObject input{{"task", spectrumMode ? "single_channel_fft_explanation" : "waveform_explanation"},
        {"local_measurements", e.facts}, {"allowed_focus", entries(e.focus)}, {"allowed_checks", entries(e.checks)},
        {"sample_selection", "At most 512 index/value pairs selected from the snapshot, not adjacent original samples; do not infer clipping, shape, frequency or harmonics from this selection."},
        {"selected_samples", samples}};
    return QString::fromUtf8(QJsonDocument(input).toJson(QJsonDocument::Compact));
}

inline QString instructions()
{
    return "你是示波器测量解释助手。测量事实和诊断边界均由本地程序计算。只选择本次最值得关注的解释及验证步骤，按优先级排序。"
        "仅输出一个 JSON 对象，恰好包含 focus_ids 和 check_ids 两个数组。focus_ids 从 allowed_focus 选择 1～6 个不同 id，"
        "check_ids 从 allowed_checks 选择 1～4 个不同 id。不要输出正文、其他字段、数值、Markdown 或自创 id。"
        "优先关注实际量程风险、DMA 已报溢出或依据不足；没有这些证据时优先解释频率、周期覆盖和幅值关系。"
        "未报告溢出不等于状态未知，也不等于证明历史连续。帧间空档不是单帧内部断点。正负峰值对称和量化台阶不能判定削顶。"
        "3000 点不自动等于周期不足，要依据估计周期数。抽取值带原始索引，不能视作按原始采样率排列的相邻点。"
        "单路 FFT 不是输入输出波特图；不能据此推断电路增益、群延迟、相位裕度、SNR、THD、功率或电路故障。";
}

inline bool incompleteResponse(const QJsonObject &root)
{
    const QJsonArray choices = root.value("choices").toArray();
    const QString finish = choices.isEmpty() ? QString() : choices.at(0).toObject().value("finish_reason").toString();
    return finish == "length" || finish == "content_filter" || root.value("status").toString() == "incomplete";
}

inline bool renderSelection(QString text, const Evidence &e, QString &rendered)
{
    text = text.trimmed();
    if (text.startsWith("```json") && text.endsWith("```")) text = text.mid(7, text.size() - 10).trimmed();
    QJsonParseError error;
    const QJsonDocument doc = QJsonDocument::fromJson(text.toUtf8(), &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject()) return false;
    const QJsonObject obj = doc.object();
    if (obj.size() != 2 || !obj.value("focus_ids").isArray() || !obj.value("check_ids").isArray()) return false;
    const auto select = [](const QJsonArray &ids, const QMap<QString, QString> &allowed, int maximum, QString &out) {
        if (ids.isEmpty() || ids.size() > maximum) return false;
        QSet<QString> seen;
        for (const auto &value : ids) {
            const QString id = value.toString();
            if (!value.isString() || !allowed.contains(id) || seen.contains(id)) return false;
            seen.insert(id);
            out += "- " + allowed.value(id) + "\n";
        }
        return true;
    };
    QString focus, checks;
    if (!select(obj.value("focus_ids").toArray(), e.focus, 6, focus)
        || !select(obj.value("check_ids").toArray(), e.checks, 4, checks)) return false;
    rendered = e.localReport + "### AI 关注点（依据本地结果）\n\n" + focus
        + "\n### 建议验证步骤\n\n" + checks;
    return true;
}
}
#endif
