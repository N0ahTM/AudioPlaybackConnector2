#include <pch.h>

#include <services/ToastContentBuilder.hpp>
#include <services/ToastXmlSanitizer.hpp>

namespace {
std::wstring_view AsView(winrt::hstring const& value) {
    return std::wstring_view(value.c_str(), value.size());
}

std::wstring UrlDecodeComponent(std::wstring_view value) {
    std::wstring normalized(value);
    std::replace(normalized.begin(), normalized.end(), L'+', L' ');

    try {
        return std::wstring(winrt::Windows::Foundation::Uri::UnescapeComponent(winrt::hstring(normalized)));
    } catch (...) {
        return normalized;
    }
}
} // namespace

ToastArguments& ToastArguments::Add(std::wstring_view key, std::wstring_view value) {
    if (!m_value.empty()) {
        m_value += L'&';
    }
    m_value += EscapeComponent(key);
    m_value += L'=';
    m_value += EscapeComponent(value);
    return *this;
}

ToastArguments& ToastArguments::Action(std::wstring_view action) {
    return Add(L"action", action);
}

ToastArguments& ToastArguments::DeviceId(winrt::hstring const& deviceId) {
    if (!deviceId.empty()) {
        Add(L"deviceId", AsView(deviceId));
    }
    return *this;
}

winrt::hstring ToastArguments::ToHString() const {
    return winrt::hstring(m_value);
}

std::unordered_map<std::wstring, std::wstring> ToastArguments::Parse(winrt::hstring const& rawArguments) {
    std::unordered_map<std::wstring, std::wstring> result;
    std::wstring_view raw(rawArguments.c_str(), rawArguments.size());

    size_t start = 0;
    while (start <= raw.size()) {
        const auto separator = raw.find(L'&', start);
        const auto end = separator == std::wstring_view::npos ? raw.size() : separator;
        const auto pair = raw.substr(start, end - start);

        if (!pair.empty()) {
            const auto equals = pair.find(L'=');
            const auto key = equals == std::wstring_view::npos ? pair : pair.substr(0, equals);
            const auto value = equals == std::wstring_view::npos ? std::wstring_view{} : pair.substr(equals + 1);
            auto decodedKey = UrlDecodeComponent(key);
            if (!decodedKey.empty()) {
                result[std::move(decodedKey)] = UrlDecodeComponent(value);
            }
        }

        if (separator == std::wstring_view::npos) break;
        start = separator + 1;
    }

    return result;
}

std::optional<std::wstring> ToastArguments::Find(std::unordered_map<std::wstring, std::wstring> const& arguments,
                                                 std::wstring_view key) {
    auto it = arguments.find(std::wstring(key));
    if (it == arguments.end() || it->second.empty()) return std::nullopt;
    return it->second;
}

std::wstring ToastArguments::EscapeComponent(std::wstring_view value) {
    return std::wstring(winrt::Windows::Foundation::Uri::EscapeComponent(winrt::hstring(std::wstring(value))));
}

ToastXmlBuilder& ToastXmlBuilder::Title(std::wstring_view value) {
    m_title = value;
    return *this;
}

ToastXmlBuilder& ToastXmlBuilder::Body(std::wstring_view value) {
    m_body = value;
    return *this;
}

ToastXmlBuilder& ToastXmlBuilder::Caption(std::wstring_view value) {
    m_caption = value;
    return *this;
}

ToastXmlBuilder& ToastXmlBuilder::Action(std::wstring_view text, ToastArguments const& arguments) {
    m_actionText = text;
    m_actionArguments = arguments.ToHString();
    return *this;
}

ToastXmlBuilder& ToastXmlBuilder::AppLogoOverride(std::wstring_view value) {
    m_appLogoOverride = value;
    return *this;
}

ToastXmlBuilder& ToastXmlBuilder::Audio(std::wstring_view src) {
    m_audioSrc = src;
    m_silentAudio = false;
    return *this;
}

ToastXmlBuilder& ToastXmlBuilder::SilentAudio() {
    m_audioSrc.clear();
    m_silentAudio = true;
    return *this;
}

ToastXmlBuilder& ToastXmlBuilder::Duration(std::wstring_view value) {
    m_duration = value;
    return *this;
}

std::wstring ToastXmlBuilder::Build() const {
    std::wstring xml = L"<toast";
    if (!m_duration.empty()) {
        xml += L" duration=\"";
        xml += apc::toast::EscapeXml(m_duration);
        xml += L"\"";
    }
    xml += L"><visual><binding template=\"ToastGeneric\">";
    if (!m_appLogoOverride.empty()) {
        xml += L"<image placement=\"appLogoOverride\" hint-crop=\"circle\" src=\"";
        xml += apc::toast::EscapeXml(m_appLogoOverride);
        xml += L"\"/>";
    }
    AppendText(xml, m_title);
    if (!m_body.empty()) {
        AppendText(xml, m_body);
    }
    if (!m_caption.empty()) {
        AppendText(xml, m_caption, L" hint-style=\"caption\" hint-color=\"secondary\"");
    }
    xml += L"</binding></visual>";
    if (!m_actionText.empty()) {
        xml += L"<actions><action content=\"";
        xml += apc::toast::EscapeXml(m_actionText);
        xml += L"\" arguments=\"";
        xml += apc::toast::EscapeXml(AsView(m_actionArguments));
        xml += L"\"/></actions>";
    }
    if (m_silentAudio) {
        xml += L"<audio silent=\"true\"/>";
    } else if (!m_audioSrc.empty()) {
        xml += L"<audio src=\"";
        xml += apc::toast::EscapeXml(m_audioSrc);
        xml += L"\"/>";
    }
    xml += L"</toast>";
    return xml;
}

void ToastXmlBuilder::AppendText(std::wstring& xml, std::wstring_view text, std::wstring_view attributes) {
    xml += L"<text";
    xml += attributes;
    xml += L">";
    xml += apc::toast::EscapeXml(text);
    xml += L"</text>";
}
