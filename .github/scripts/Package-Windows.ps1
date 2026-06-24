[CmdletBinding()]
param(
    [ValidateSet('x64')]
    [string] $Target = 'x64',
    [ValidateSet('Debug', 'RelWithDebInfo', 'Release', 'MinSizeRel')]
    [string] $Configuration = 'RelWithDebInfo',
    [switch] $Package
)

$ErrorActionPreference = 'Stop'

if ( $DebugPreference -eq 'Continue' ) {
    $VerbosePreference = 'Continue'
    $InformationPreference = 'Continue'
}

if ( $env:CI -eq $null ) {
    throw "Package-Windows.ps1 requires CI environment"
}

if ( ! ( [System.Environment]::Is64BitOperatingSystem ) ) {
    throw "Packaging script requires a 64-bit system to build and run."
}

if ( $PSVersionTable.PSVersion -lt '7.2.0' ) {
    Write-Warning 'The packaging script requires PowerShell Core 7. Install or upgrade your PowerShell version: https://aka.ms/pscore6'
    exit 2
}

function Get-InnoSetupCompiler {
    $Iscc = Get-Command iscc -ErrorAction SilentlyContinue
    if ( $Iscc ) {
        return $Iscc.Source
    }

    $CandidatePaths = @(
        "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe"
        "${env:ProgramFiles}\Inno Setup 6\ISCC.exe"
    )

    foreach ( $Path in $CandidatePaths ) {
        if ( Test-Path -Path $Path ) {
            return $Path
        }
    }

    throw 'Inno Setup compiler (iscc) not found. Install Inno Setup 6 or add iscc to PATH.'
}

function New-PortablePackage {
    param(
        [string] $InstallRoot,
        [string] $ProductName,
        [string] $StagingDir
    )

    if ( ! ( Test-Path -Path $InstallRoot ) ) {
        throw "Plugin install directory not found: ${InstallRoot}"
    }

    $BinSource = Join-Path $InstallRoot 'bin/64bit'
    $DataSource = Join-Path $InstallRoot 'data'

    if ( ! ( Test-Path -Path $BinSource ) ) {
        throw "Plugin binaries not found: ${BinSource}"
    }

    $ObsPluginsDir = Join-Path $StagingDir 'obs-plugins/64bit'
    $PluginDataDir = Join-Path $StagingDir "data/obs-plugins/${ProductName}"

    New-Item -ItemType Directory -Force -Path $ObsPluginsDir | Out-Null

    Copy-Item -Path (Join-Path $BinSource '*') -Destination $ObsPluginsDir -Force

    if ( Test-Path -Path $DataSource ) {
        New-Item -ItemType Directory -Force -Path $PluginDataDir | Out-Null
        Copy-Item -Path (Join-Path $DataSource '*') -Destination $PluginDataDir -Recurse -Force
    }
}

function Package {
    trap {
        Write-Error $_
        exit 2
    }

    $ScriptHome = $PSScriptRoot
    $ProjectRoot = Resolve-Path -Path "$PSScriptRoot/../.."
    $BuildSpecFile = "${ProjectRoot}/buildspec.json"

    $UtilityFunctions = Get-ChildItem -Path $PSScriptRoot/utils.pwsh/*.ps1 -Recurse

    foreach( $Utility in $UtilityFunctions ) {
        Write-Debug "Loading $($Utility.FullName)"
        . $Utility.FullName
    }

    $BuildSpec = Get-Content -Path ${BuildSpecFile} -Raw | ConvertFrom-Json
    $ProductName = $BuildSpec.name
    $ProductVersion = $BuildSpec.version

    $InstallRoot = Join-Path $ProjectRoot "release/${Configuration}/${ProductName}"
    $PortableOutputName = "${ProductName}-${ProductVersion}-windows-${Target}-portable"
    $InstallerOutputName = "${ProductName}-${ProductVersion}-windows-${Target}-Installer"

    $RemoveArgs = @{
        ErrorAction = 'SilentlyContinue'
        Path = @(
            "${ProjectRoot}/release/${ProductName}-*-windows-*.zip"
            "${ProjectRoot}/release/${ProductName}-*-windows-*-Installer.exe"
            "${ProjectRoot}/release/.staging-portable"
            "${ProjectRoot}/release/Package"
        )
    }

    Remove-Item @RemoveArgs -Recurse -Force

    Log-Group "Creating portable archive for ${ProductName}..."
    $PortableStagingDir = Join-Path $ProjectRoot 'release/.staging-portable'
    New-PortablePackage -InstallRoot $InstallRoot -ProductName $ProductName -StagingDir $PortableStagingDir

    $CompressArgs = @{
        Path = @(
            (Join-Path $PortableStagingDir 'obs-plugins')
            (Join-Path $PortableStagingDir 'data')
        )
        CompressionLevel = 'Optimal'
        DestinationPath = "${ProjectRoot}/release/${PortableOutputName}.zip"
        Verbose = ($Env:CI -ne $null)
    }
    Compress-Archive -Force @CompressArgs
    Remove-Item -Path $PortableStagingDir -Recurse -Force
    Log-Group

    if ( $Package ) {
        $IsccFile = Join-Path $ProjectRoot "build_${Target}/installer.iss"

        if ( ! ( Test-Path -Path $IsccFile ) ) {
            throw 'InnoSetup install script not found. Run the build script or the CMake build and install procedures first.'
        }

        Log-Group "Creating installer for ${ProductName}..."
        Push-Location -Stack BuildTemp

        Ensure-Location -Path "${ProjectRoot}/release"
        New-Item -ItemType Directory -Force -Path Package | Out-Null
        Copy-Item -Path (Join-Path $InstallRoot '*') -Destination Package -Recurse -Force

        $Iscc = Get-InnoSetupCompiler
        Invoke-External $Iscc $IsccFile /O"${ProjectRoot}/release" /F"${InstallerOutputName}"

        Remove-Item -Path Package -Recurse -Force
        Pop-Location -Stack BuildTemp
        Log-Group
    }
}

Package
