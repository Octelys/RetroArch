# Disabled GitHub Actions workflows

This directory archives workflow definitions that were intentionally deactivated.

Only the following build targets remain active under `.github/workflows/`:

- Windows ARM64 via `Windows-ARM64.yml`
- Windows x64 via `Windows-x64-MXE.yml`
- macOS via `MacOS.yml`
- Linux via `Linux.yml`

The `MSVC.yml`, `MSYS2.yml`, and `retroarch.yml` workflows are archived here as alternate or duplicate builds that are no longer active.

GitHub Actions only loads workflow files from `.github/workflows/`, so keeping archived workflows here preserves their history without running them.

