#ifndef AI_API_H
#define AI_API_H

#include <QJsonObject>
#include <QJsonArray>
#include <QStringList>
#include <QUrl>
#include <QJsonDocument>

namespace AiApi {
enum Provider { DeepSeek, Gemini, OpenAI };

// Gateways may return plain text or a different JSON error envelope.
inline QString errorMessage(const QByteArray &payload, const QString &fallback,
                            const QString &key, int status)
{
    const QJsonObject root = QJsonDocument::fromJson(payload).object();
    const QJsonValue error = root.value("error");
    QString detail = error.toObject().value("message").toString();
    if (detail.isEmpty()) detail = error.toString();
    if (detail.isEmpty()) detail = root.value("message").toString();
    if (detail.isEmpty()) detail = root.value("detail").toString();
    if (detail.isEmpty()) detail = QString::fromUtf8(payload).trimmed();
    if (detail.isEmpty()) detail = fallback + QStringLiteral("\n服务器未返回错误详情。");
    QString hint;
    if (status == 400)
        hint = QStringLiteral("服务器拒绝请求，请根据下面的详情检查模型名称及请求参数。");
    else if (status == 401)
        hint = QStringLiteral("密钥验证失败，请确认使用的是所选服务商的完整 API Key。");
    else if (status == 402)
        hint = QStringLiteral("账户余额不足，请检查所选平台的 API 账户余额。");
    else if (status == 429)
        hint = QStringLiteral("额度或调用频率达到限制，请稍后再试并检查账户额度。");
    QString message = QStringLiteral("分析失败（HTTP %1）\n%2\n\n%3")
        .arg(status > 0 ? QString::number(status) : QStringLiteral("无状态码"), hint, detail);
    if (!key.isEmpty()) message.replace(key, QStringLiteral("[已隐藏密钥]"));
    return message.left(2000);
}

inline QUrl endpoint(Provider provider)
{
    if (provider == DeepSeek)
        return QUrl("https://api.deepseek.com/chat/completions");
    return QUrl(provider == Gemini
        ? "https://generativelanguage.googleapis.com/v1beta/openai/chat/completions"
        : "https://api.openai.com/v1/responses");
}

inline QJsonObject requestBody(Provider provider, const QString &model,
                               const QString &instructions, const QString &input, int maxOutputTokens = 600)
{
    QJsonObject body;
    body.insert("model", model);
    if (provider != OpenAI) {
        body.insert("messages", QJsonArray{
            QJsonObject{{"role", "system"}, {"content", instructions}},
            QJsonObject{{"role", "user"}, {"content", input}}});
        body.insert("max_tokens", maxOutputTokens);
        body.insert("stream", false);
        if (provider == DeepSeek)
            body.insert("thinking", QJsonObject{{"type", "disabled"}});
    } else {
        body.insert("store", false);
        body.insert("max_output_tokens", maxOutputTokens);
        body.insert("instructions", instructions);
        body.insert("input", input);
    }
    return body;
}

inline QString responseText(Provider provider, const QJsonObject &root)
{
    if (provider != OpenAI) {
        const QJsonArray choices = root.value("choices").toArray();
        if (choices.isEmpty()) return QString();
        return choices.first().toObject().value("message").toObject()
            .value("content").toString();
    }
    QStringList texts;
    for (const QJsonValue &item : root.value("output").toArray()) {
        const QJsonObject output = item.toObject();
        if (output.value("type").toString() != "message") continue;
        for (const QJsonValue &part : output.value("content").toArray()) {
            const QJsonObject content = part.toObject();
            if (content.value("type").toString() == "output_text")
                texts << content.value("text").toString();
        }
    }
    return texts.join("\n\n");
}
}
#endif
