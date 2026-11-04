param(
    [Parameter(Mandatory=$true)][string]$AssemblyPath,
    [string]$OutputDirectory,
    [string]$DiagnosticsDirectory,
    [switch]$IncludeElectronics
)
$ErrorActionPreference='Stop'
if ($PSVersionTable.PSEdition -ne 'Desktop') { throw 'Run with Windows PowerShell 5.1.' }
if (-not $OutputDirectory) { $OutputDirectory = Join-Path (Split-Path -Parent $PSScriptRoot) 'dist\assets' }
if (-not $DiagnosticsDirectory) { $DiagnosticsDirectory = Join-Path $PSScriptRoot 'cad-output' }
[void](New-Item -ItemType Directory -Force -Path $OutputDirectory)
[void](New-Item -ItemType Directory -Force -Path $DiagnosticsDirectory)
$interop='D:\04_Mechanical_App\SOLIDWORKS2026\SOLIDWORKS\api\redist\SolidWorks.Interop.sldworks.dll'
Add-Type -Path $interop
Add-Type -TypeDefinition (Get-Content -LiteralPath (Join-Path $PSScriptRoot 'cad_export.cs') -Raw -Encoding UTF8) -ReferencedAssemblies @($interop,'System.Web.Extensions')
[CadReadOnlyExport]::Run($AssemblyPath, $OutputDirectory, $DiagnosticsDirectory, $IncludeElectronics.IsPresent)
