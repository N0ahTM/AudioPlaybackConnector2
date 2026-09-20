#pragma once

#include <winrt/base.h>

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

class ToastArguments final {
public:
    ToastArguments& Add(std::wstring_view key, std::wstring_view value);
    ToastArguments& Action(std::wstring_view action);
    ToastArguments& DeviceId(winrt::hstring const& deviceId);

    [[nodiscard]] winrt::hstring ToHString() const;
    [[nodiscard]] static std::unordered_map<std::wstring, std::wstring> Parse(winrt::hstring const& rawArguments);
    [[nodiscard]] static std::optional<std::wstring>
    Find(std::unordered_map<std::wstring, std::wstring> const& arguments, std::wstring_view key);

private:
    [[nodiscard]] static std::wstring EscapeComponent(std::wstring_view value);

    std::wstring m_value;
};

class ToastXmlBuilder final {
public:
    ToastXmlBuilder& Title(std::wstring_view value);
    ToastXmlBuilder& Body(std::wstring_view value);
    ToastXmlBuilder& Caption(std::wstring_view value);
    ToastXmlBuilder& Action(std::wstring_view text, ToastArguments const& arguments);
    ToastXmlBuilder& AppLogoOverride(std::wstring_view value);
    ToastXmlBuilder& Audio(std::wstring_view src);
    ToastXmlBuilder& SilentAudio();
    ToastXmlBuilder& Duration(std::wstring_view value);

    [[nodiscard]] std::wstring Build() const;

private:
    static void AppendText(std::wstring& xml, std::wstring_view text, std::wstring_view attributes = {});

    std::wstring m_title;
    std::wstring m_body;
    std::wstring m_caption;
    std::wstring m_actionText;
    winrt::hstring m_actionArguments;
    std::wstring m_appLogoOverride;
    std::wstring m_audioSrc;
    std::wstring m_duration;
    bool m_silentAudio = false;
};
