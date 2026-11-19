# cast_bridge

A new Flutter project.

## Getting Started

This project is a starting point for a Flutter application.

A few resources to get you started if this is your first Flutter project:

- [Learn Flutter](https://docs.flutter.dev/get-started/learn-flutter)
- [Write your first Flutter app](https://docs.flutter.dev/get-started/codelab)
- [Flutter learning resources](https://docs.flutter.dev/reference/learning-resources)

For help getting started with Flutter development, view the
[online documentation](https://docs.flutter.dev/), which offers tutorials,
samples, guidance on mobile development, and a full API reference.

## iOS cloud build

The GitHub Actions workflow in `.github/workflows/ios-build.yml` uses a hosted
macOS runner and Xcode to analyze, test, and build the iOS app without code
signing. It runs on pushes, pull requests, or manual dispatch. The unsigned
`Runner.app` is available as the `castbridge-ios-unsigned` workflow artifact.

The project must be pushed to a GitHub repository before Actions can run. An
Apple Developer signing setup is required to produce an installable IPA or
submit a build to the App Store.

## Windows cloud build

The GitHub Actions workflow in `.github/workflows/windows-build.yml` uses a
hosted Windows runner to analyze, test, and build the x64 desktop app. The
packaged `castbridge-windows-x64` artifact contains the app and its runtime
files and can be downloaded from the workflow run.
