# Translating AudioPlaybackConnector2

English `en.json` is canonical. All eight committed locales must contain exactly its keys and matching placeholders. Runtime English fallback remains available when a language resource cannot be loaded. Files under `AudioPlaybackConnector2/res/strings/` are UTF-8 JSON objects of non-empty strings.

## Supported locales

| Language | File | Setting |
| --- | --- | --- |
| English | en.json | en |
| German | de.json | de |
| French | fr.json | fr |
| Spanish | es.json | es |
| Japanese | ja.json | ja |
| Korean | ko.json | ko |
| Simplified Chinese | zh_hans.json | zh_hans |
| Traditional Chinese | zh_hant.json | zh_hant |

## Improve an existing language

1. Branch from `main`; edit the target locale (English only if its meaning is wrong).
2. Preserve keys, exact placeholders (`{0}`, `{1}`, `{0:08X}`), intentional `\n`, accelerators and punctuation meaning. Use concise, natural UI wording.
3. Run `pwsh ./scripts/validate-localizations.ps1`; build/select the language and inspect affected controls at normal text scaling.
4. Open a PR naming language, checked screens and native/fluent review. Disclose machine-assisted drafts; fluent review is required before release. Do not mass-copy machine translations into other locales.

## Add or change an English string

Add a stable descriptive key to `en.json`; do not reuse an unrelated key for matching wording. Update all eight locale tables together and run the validator before opening the PR; missing keys fail the quality gate. Coordinate translations in [Discussions](https://github.com/N0ahTM/AudioPlaybackConnector2/discussions).

## Add a new language

Discuss naming, Windows mapping and maintenance first. In one PR, add locale JSON, ID in `res/resource.h`, embedding in `res/AudioPlaybackConnector2.rc`, setting/Windows-language mapping in StringResources.cpp, Settings choice and translated language names. Extend the validator, update the README language list, decide Store-notes support, build x64/ARM64 and test the packaged UI. Resource paths are relative to `AudioPlaybackConnector2/`.
