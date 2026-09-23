#include "TestCheck.hpp"
#include <services/ToastContentBuilder.hpp>
#include <services/ToastXmlSanitizer.hpp>
#include <winrt/Windows.Data.Xml.Dom.h>
#include <array>
#include <random>
#include <string>

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Toast Boundary Tests //////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

namespace {
void TestArgumentRoundTrips() {
    constexpr std::array samples{L"plain",
                                 L"",
                                 L"a+b & action=retry",
                                 L"%26action%3Dretry",
                                 L"\"'<>&=?#",
                                 L" leading and trailing ",
                                 L"Gr\u00fc\u00dfe \u65e5\u672c\u8a9e \ud55c\uad6d\uc5b4",
                                 L"\U0001F600"};
    for (auto value : samples) {
        auto decoded = ToastArguments::Parse(ToastArguments{}.Action(L"reconnect").Add(L"deviceId", value).ToHString());
        Check(decoded.size() == 2 && decoded.at(L"action") == L"reconnect" && decoded.at(L"deviceId") == value,
              "Unicode and delimiters must round-trip without injecting fields");
    }
    auto absent = ToastArguments::Parse(ToastArguments{}.Action(L"retry").DeviceId(L"").ToHString());
    Check(absent.size() == 1 && !ToastArguments::Find(absent, L"deviceId"), "empty device ID must not supply a target");
    // Fixed generator/output mapping is reproducible across standard libraries.
    std::mt19937 random(920040);
    constexpr std::array alphabet{
        L"a", L"&", L"=", L"+", L"%", L" ", L"\"", L"'", L"<", L">", L"\u00e4", L"\u4e2d", L"\U0001F600"};
    for (int sample = 0; sample < 1000; ++sample) {
        std::wstring id;
        const auto length = random() % 80 + 1;
        for (unsigned index = 0; index < length; ++index)
            id += alphabet[random() % alphabet.size()];
        auto decoded =
            ToastArguments::Parse(ToastArguments{}.Action(L"retry").DeviceId(winrt::hstring(id)).ToHString());
        Check(decoded.size() == 2 && ToastArguments::Find(decoded, L"action") == L"retry" &&
                  ToastArguments::Find(decoded, L"deviceId") == id,
              "generated activation corpus seed 920040 must round-trip");
    }
}

void TestArgumentParsingBoundaries() {
    auto parsed = ToastArguments::Parse(L"&&=ignored&action=reconnect&deviceId=a%26b%3Dc%2Bd+e&&");
    Check(parsed.size() == 2 && ToastArguments::Find(parsed, L"deviceId") == L"a&b=c+d e",
          "split fields before decoding; encoded plus is literal while raw plus is a space");
    auto once = ToastArguments::Parse(L"deviceId=%2526action%253Dretry");
    Check(once.size() == 1 && ToastArguments::Find(once, L"deviceId") == L"%26action%3Dretry",
          "activation values must be decoded exactly once");
    auto empty = ToastArguments::Parse(L"action=&deviceId&unknown=value");
    Check(!ToastArguments::Find(empty, L"action") && !ToastArguments::Find(empty, L"deviceId") &&
              !ToastArguments::Find(empty, L"absent"),
          "missing and empty fields must not supply actionable values");
    for (auto malformed : {L"%", L"%0", L"%GG", L"%C0%AF", L"%ED%A0%80"}) {
        auto decoded = ToastArguments::Parse(winrt::hstring(std::wstring(L"deviceId=") + malformed));
        Check(decoded.size() == 1 && !ToastArguments::Find(decoded, L"action"),
              "malformed percent input must neither throw nor manufacture an action");
    }
}

void TestXmlStructureAndActivation() {
    using winrt::Windows::Data::Xml::Dom::XmlDocument;
    using winrt::Windows::Data::Xml::Dom::XmlElement;
    const std::wstring hostile = L"\"/><action content='injected'/><text>& payload";
    auto arguments = ToastArguments{}.Action(L"retry").DeviceId(winrt::hstring(hostile));
    auto builder = ToastXmlBuilder{};
    builder.Title(hostile)
        .Body(hostile)
        .Caption(hostile)
        .AppLogoOverride(hostile)
        .Action(hostile, arguments)
        .Audio(hostile)
        .Duration(hostile);
    XmlDocument document;
    document.LoadXml(builder.Build());
    Check(document.SelectNodes(L"/toast").Length() == 1 &&
              document.SelectNodes(L"/toast/visual/binding/text").Length() == 3 &&
              document.SelectNodes(L"//action").Length() == 1,
          "untrusted text must not add XML elements");
    Check(document.SelectSingleNode(L"/toast/visual/binding/text[1]").InnerText() == hostile &&
              document.DocumentElement().GetAttribute(L"duration") == hostile &&
              document.SelectSingleNode(L"//image").as<XmlElement>().GetAttribute(L"src") == hostile &&
              document.SelectSingleNode(L"//audio").as<XmlElement>().GetAttribute(L"src") == hostile,
          "XML parser must recover original text and attribute values");
    auto action = document.SelectSingleNode(L"//action").as<XmlElement>();
    auto decoded = ToastArguments::Parse(action.GetAttribute(L"arguments"));
    Check(action.GetAttribute(L"content") == hostile && ToastArguments::Find(decoded, L"deviceId") == hostile &&
              ToastArguments::Find(decoded, L"action") == L"retry",
          "XML and URL encoding must preserve the activation target");
    builder.SilentAudio();
    document.LoadXml(builder.Build());
    auto audio = document.SelectSingleNode(L"//audio").as<XmlElement>();
    Check(audio.GetAttribute(L"silent") == L"true" && document.SelectNodes(L"//audio/@src").Length() == 0,
          "silent audio must replace source");
    builder.Audio(L"ms-winsoundevent:Notification.Default");
    document.LoadXml(builder.Build());
    Check(document.SelectNodes(L"//audio/@silent").Length() == 0, "audio must clear silent mode");
    document.LoadXml(ToastXmlBuilder{}.Title(L"only title").Build());
    Check(document.SelectNodes(L"//text").Length() == 1 && document.SelectNodes(L"//action").Length() == 0 &&
              document.SelectNodes(L"//image").Length() == 0 && document.SelectNodes(L"//audio").Length() == 0,
          "unset optional content must not create empty actions or media");
}

void TestToastXmlSanitization() {
    Check(apc::toast::EscapeXml(L"&<>\"'\t\n\r") == L"&amp;&lt;&gt;&quot;&apos;\t\n\r",
          "toast XML metacharacters must be escaped without removing legal whitespace");

    std::wstring invalid{L'A',
                         L'\0',
                         static_cast<wchar_t>(0x1F),
                         static_cast<wchar_t>(0xD800),
                         L'B',
                         static_cast<wchar_t>(0xDC00),
                         L'C',
                         static_cast<wchar_t>(0xFFFE)};
    Check(apc::toast::EscapeXml(invalid) == L"A\uFFFD\uFFFD\uFFFDB\uFFFDC\uFFFD",
          "illegal XML controls and unpaired UTF-16 surrogates must be replaced");

    std::wstring surrogatePair{static_cast<wchar_t>(0xD83D), static_cast<wchar_t>(0xDE00)};
    Check(apc::toast::EscapeXml(surrogatePair) == surrogatePair, "valid UTF-16 surrogate pairs must be preserved");
}

} // namespace

int RunToastContentBuilderTests() {
    TestArgumentRoundTrips();
    TestArgumentParsingBoundaries();
    TestXmlStructureAndActivation();
    TestToastXmlSanitization();
    return g_failures;
}
