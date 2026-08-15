#include <ui/SettingsDiagnosticsReport.hpp>

#include <iostream>
#include <string_view>

namespace {
int g_failures = 0;

void Check(bool condition, std::string_view message) {
    if (condition) return;
    ++g_failures;
    std::cerr << "FAILED: " << message << '\n';
}

apc::ui::SettingsDiagnosticsReportContext Context() {
    apc::ui::SettingsDiagnosticsReportContext context;
    context.LogUnavailable = L"log unavailable";
    context.NoRecentErrors = L"no recent errors";
    context.LogEntriesOmitted = L"entries omitted";
    return context;
}

void TestSkippedEntriesAreNotReportedAsNoErrors() {
    auto context = Context();
    apc::ui::DiagnosticsLogResult logResult;
    logResult.Status = apc::ui::DiagnosticsLogStatus::Available;
    logResult.SkippedMalformedOrOversized = true;
    auto report = apc::ui::BuildSettingsDiagnosticsReport({}, 0, context, logResult);
    Check(report.find(context.LogEntriesOmitted) != std::wstring::npos,
          "skipped relevant entries must be disclosed in the report");
    Check(report.find(context.NoRecentErrors) == std::wstring::npos,
          "skipped relevant entries must not be described as no recent errors");
}

void TestFinalReportSizeIsBounded() {
    auto context = Context();
    context.Title.assign(apc::ui::c_settingsDiagnosticsReportMaxCharacters + 1, L'x');
    auto report = apc::ui::BuildSettingsDiagnosticsReport({}, 0, context, {});
    Check(report.empty(), "a report above the final character cap must be rejected");
}
} // namespace

int RunSettingsDiagnosticsReportBuilderTests() {
    TestSkippedEntriesAreNotReportedAsNoErrors();
    TestFinalReportSizeIsBounded();
    return g_failures;
}
