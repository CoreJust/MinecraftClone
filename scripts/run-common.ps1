<#
.DESCRIPTION
  A set of common function used in other run-* scripts.
#>

$ErrorActionPreference = "Stop"

# Message printing
function Info($m){ Write-Host "[..] $m" -ForegroundColor Cyan }
function Ok($m){ Write-Host "[OK] $m" -ForegroundColor Green }
function Err($m){ Write-Host "[ERR] $m" -ForegroundColor Red }

# Run command helper
function Run {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true, Position = 0)][string]$Exe,
        [Parameter(Position = 1)][string[]]$Arguments = @(),
        [Parameter(Position = 2)][string]$WorkingDirectory = $null
    )
    process {
        $displayArgs = $Arguments -join ' '
        Write-Verbose "Running: $Exe $displayArgs"
        if ($WorkingDirectory) { Write-Verbose "Working directory: $WorkingDirectory" }

        $params = @{
            FilePath     = $Exe
            ArgumentList = $Arguments
            NoNewWindow  = $true
            Wait         = $true
            PassThru     = $true
            ErrorAction  = 'Stop'
        }

        if ($WorkingDirectory) { $params.WorkingDirectory = $WorkingDirectory }
        try {
            $process = Start-Process @params
            if ($process.ExitCode -ne 0) {
                throw "Command '$Exe' failed with exit code $($process.ExitCode)"
            }
        }
        catch {
            Write-Error $_.Exception.Message
            throw
        }
    }
}

# Checks the tool is available
function Require-Tool($name) {
  if (-not (Get-Command $name -ErrorAction SilentlyContinue)) {
    throw "Required tool '$name' not found in PATH"
  }
  Ok "$name available"
}

function Prepare-Vcpkg {
  $root = Join-Path $PSScriptRoot ../$VcpkgDir
  $exe  = Join-Path $root "vcpkg.exe"
  $tc   = Join-Path $root "scripts/buildsystems/vcpkg.cmake"

  if (-not (Test-Path $exe)) {
    if (-not (Test-Path $root)) {
      Info "Cloning vcpkg..."
      & git clone --depth 1 https://github.com/microsoft/vcpkg.git $root
      if ($LASTEXITCODE -ne 0) { throw "Failed to clone vcpkg" }
    }
    Info "Bootstrapping vcpkg..."
    Push-Location $root
    try {
      & .\bootstrap-vcpkg.bat
      if ($LASTEXITCODE -ne 0) { throw "vcpkg bootstrap failed" }
    } finally {
      Pop-Location
    }
  }

  if (-not (Test-Path $exe)) { throw "vcpkg.exe not found after bootstrap" }
  if (-not (Test-Path $tc))  { throw "vcpkg toolchain file missing" }

  Ok "vcpkg ready at $root"
  return @{
    Root = $root
    Exe  = $exe
    Toolchain = $tc
  }
}

function Install-VcpkgDependenciesIfNeeded {
  param(
    [string]$ManifestDir,
    [string]$InstallRoot,
    [string]$VcpkgExe
  )
  
  $manifestFile = Join-Path $ManifestDir "vcpkg.json"
  $lockFile = Join-Path $ManifestDir "vcpkg.lock"
  $projectName = Split-Path $ManifestDir -Leaf
  $stateFile = Join-Path $InstallRoot "state_$projectName.txt"
  
  $currentHash = ""
  if (Test-Path $manifestFile) {
    $manifestHash = Get-FileHash $manifestFile -Algorithm SHA256 | Select-Object -ExpandProperty Hash
    $currentHash += $manifestHash
  }
  if (Test-Path $lockFile) {
    $lockHash = Get-FileHash $lockFile -Algorithm SHA256 | Select-Object -ExpandProperty Hash
    $currentHash += $lockHash
  }
  
  $previousHash = ""
  if (Test-Path $stateFile) {
    $previousHash = Get-Content $stateFile -Raw
  }
  
  if ($currentHash -ne $previousHash -or -not (Test-Path $InstallRoot)) {
    Info "Installing $projectName deps..."
    & $VcpkgExe install `
      --triplet x64-windows `
      --x-manifest-root $ManifestDir `
      --x-install-root $InstallRoot
    
    if ($LASTEXITCODE -ne 0) { throw "vcpkg install failed for $projectName" }
    
    $currentHash | Set-Content $stateFile -Force
    Ok "$projectName dependencies installed"
  } else {
    Ok "$projectName dependencies are already up to date"
  }
}
