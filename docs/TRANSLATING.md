# Translating AudioPlaybackConnector2

Translations are ordinary pull requests. English (`en.json`) is the source language; every other locale overlays English at runtime. A missing translated key therefore displays English instead of an empty label.

## Supported locales

| Language | File | Internal setting |
| --- | --- | --- |
| English | `en.json` | `en` |
| German | `de.json` | `de` |
| French | `fr.json` | `fr` |
| Spanish | `es.json` | `es` |
| Japanese | `ja.json` | `ja` |
| Korean | `ko.json` | `ko` |
| Chinese, Simplified | `zh_hans.json` | `zh_hans` |
| Chinese, Traditional | `zh_hant.json` | `zh_hant` |

Files live in `AudioPlaybackConnector2/res/strings/` and must remain UTF-8 JSON objects whose values are non-empty strings.

## Improve an existing language

1. Fork the repository and create a branch from `main`.
2. Edit only the target locale unless the English meaning is also wrong.
3. Preserve keys and formatting placeholders such as `{0}`, `{1}`, and `{0:08X}` exactly.
4. Preserve intentional `\n` line breaks and accelerator or punctuation meaning.
5. Prefer natural, concise UI language over a word-for-word translation.
6. Run `pwsh ./scripts/validate-localizations.ps1`.
7. Build the app, select the language, and inspect affected controls at normal Windows text scaling.
8. Open a pull request describing the language, whether a native or fluent speaker reviewed it, and which screens were checked.

A translation PR may update one language. Do not copy a machine translation into every locale. Machine assistance is acceptable as a draft only when the pull request says so and a fluent reviewer verifies it before release.

## Add or change an English string

Use a stable, descriptive key and add it to `en.json`. Existing locales may temporarily fall back to English, but user-facing releases should aim for complete translations. Do not reuse an unrelated key merely because its current English wording happens to match.

Before release, review translation completeness printed by `validate-localizations.ps1` and coordinate missing text in the [Translations discussion](https://github.com/N0ahTM/AudioPlaybackConnector2/discussions).

## Add a new language

A new language affects more than one JSON file. The same pull request must:

1. add the locale JSON under `AudioPlaybackConnector2/res/strings/`;
2. add a resource ID in `AudioPlaybackConnector2/res/resource.h`;
3. embed it in `AudioPlaybackConnector2/res/AudioPlaybackConnector2.rc`;
4. map the explicit setting and Windows UI language in `StringResources.cpp`;
5. expose the language in Settings and its localized language-name resources;
6. extend `scripts/validate-localizations.ps1`;
7. update the README language list;
8. decide whether Microsoft Store release notes also support the locale;
9. build x64 and ARM64 and test the language in the packaged app.

Open a discussion before adding a language so file names, Windows language mapping, and ongoing maintenance can be agreed on first.
