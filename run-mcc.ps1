<#
.DESCRIPTION
  Build and install Shadberry engine, then build and run
  Samples/MinecraftClone against the installed engine.

  Uses:
  - CMake >= 4.0
  - Ninja
  - vcpkg (manifest mode)
#>

[CmdletBinding()]
param(
  [string]$BuildSBDir = "build/shadberry",
  [string]$BuildMCCDir = "build/mcc",
  [string]$VcpkgDir = ".vcpkg",
  [string]$Config = "RelWithDebInfo"
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

  Info "Installing Shadberry deps..."
  Install-VcpkgDependenciesIfNeeded `
    -ManifestDir (Join-Path $PSScriptRoot "Shadberry") `
    -InstallRoot $CentralInstallRoot `
    -VcpkgExe $vcpkg.Exe

  $buildSBAbs = Join-Path $PSScriptRoot $BuildSBDir
  $buildMCCAbs = Join-Path $PSScriptRoot $BuildMCCDir
  $shadInstallAbs = "${buildSBAbs}/install"
  New-Item -ItemType Directory -Force -Path $buildSBAbs, $shadInstallAbs, $buildMCCAbs | Out-Null

  Info "Configuring Shadberry..."
  Run cmake @(
    "-S", "Shadberry",
    "-B", $buildSBAbs,
    "-G", "Ninja",
    "-DCMAKE_BUILD_TYPE=$Config",
    "-DCMAKE_TOOLCHAIN_FILE=$($vcpkg.Toolchain)",
    "-DCMAKE_INSTALL_PREFIX=$shadInstallAbs",
    "-DSHADBERRY_BUILD_TESTS=OFF"
  )

  Info "Building Shadberry..."
  Run cmake @("--build", $buildSBAbs)
  Info "Installing Shadberry..."
  Run cmake @("--install", $buildSBAbs)

  Info "Installing MinecraftClone deps..."
  Install-VcpkgDependenciesIfNeeded `
    -ManifestDir (Join-Path $PSScriptRoot "Samples/MinecraftClone") `
    -InstallRoot $CentralInstallRoot `
    -VcpkgExe $vcpkg.Exe

  Info "Configuring MinecraftClone..."
  Run cmake @(
    "-S", "Samples/MinecraftClone",
    "-B", $buildMCCAbs,
    "-G", "Ninja",
    "-DCMAKE_BUILD_TYPE=$Config",
    "-DCMAKE_PREFIX_PATH=$shadInstallAbs",
    "-DCMAKE_TOOLCHAIN_FILE=$($vcpkg.Toolchain)"
  )

  Info "Building MinecraftClone..."
  Run cmake @("--build", $BuildMCCDir)

  $exe = Join-Path $BuildMCCDir "MinecraftClone.exe"
  if (-not (Test-Path $exe)) { throw "MinecraftClone.exe not found" }

  Info "Running MinecraftClone..."
  Run $exe
  #$p = Start-Process $exe -NoNewWindow -Wait -PassThru
  #if ($p.ExitCode -ne 0) {
  #  throw "Command failed: $exe (exit $($p.ExitCode))"
  #}

  Ok "MinecraftClone finished successfully."
}
catch {
  Err $_.Exception.Message
  exit 1
}
