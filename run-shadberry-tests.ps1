<#
.DESCRIPTION
  Configure, build and run Shadberry tests using:
  - CMake >= 4.0
  - Ninja
  - vcpkg (manifest mode)
  - GoogleTest + CTest

  Script is fully self-contained:
  if vcpkg is not present locally, it will be cloned and bootstrapped.
#>

[CmdletBinding()]
param(
  [string]$BuildDir = "build/shadberry",
  [string]$VcpkgDir = ".vcpkg",
  [string]$Config   = "RelWithDebInfo"
)

$ErrorActionPreference = "Stop"

. ./scripts/run-common.ps1

try {
  Info "Checking tools..."
  Require-Tool git
  Require-Tool cmake
  Require-Tool ninja
  Require-Tool ctest
  Ok "Tools present."

  $vcpkg = Prepare-Vcpkg
  $CentralInstallRoot = Join-Path $PSScriptRoot ".vcpkg_installed"
  if (-not (Test-Path $CentralInstallRoot)) { New-Item -ItemType Directory -Force -Path $CentralInstallRoot | Out-Null }

  Info "Installing Shadberry dependencies (vcpkg manifest)..."
  Install-VcpkgDependenciesIfNeeded `
    -ManifestDir (Join-Path $PSScriptRoot "Shadberry") `
    -InstallRoot $CentralInstallRoot `
    -VcpkgExe $vcpkg.Exe

  $buildAbs = Join-Path $PSScriptRoot $BuildDir
  New-Item -ItemType Directory -Force -Path $buildAbs | Out-Null

  Info "Configuring Shadberry..."
  Run cmake @(
    "-S", "Shadberry",
    "-B", $buildAbs,
    "-G", "Ninja",
    "-DCMAKE_BUILD_TYPE=$Config",
    "-DCMAKE_TOOLCHAIN_FILE=$($vcpkg.Toolchain)",
    "-DSHADBERRY_BUILD_TESTS=ON"
  )

  Info "Building..."
  Run cmake @("--build", $buildAbs)

  Info "Running tests..."
  Run ctest @("--output-on-failure", "-C", $Config) $buildAbs

  Ok "All Shadberry tests passed."
}
catch {
  Err $_.Exception.Message
  exit 1
}
