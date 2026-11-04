param(
    [Parameter(Mandatory=$true)][string]$AssemblyPath,
    [string]$OutputPath
)
$ErrorActionPreference='Stop'
if (-not $OutputPath) { $OutputPath = Join-Path $PSScriptRoot 'cad-output\inventory.json' }
[void](New-Item -ItemType Directory -Force -Path (Split-Path -Parent $OutputPath))
if ($PSVersionTable.PSEdition -ne 'Desktop') { throw 'Run with Windows PowerShell 5.1.' }
$interop='D:\04_Mechanical_App\SOLIDWORKS2026\SOLIDWORKS\api\redist\SolidWorks.Interop.sldworks.dll'
Add-Type -Path $interop
Add-Type -TypeDefinition (Get-Content -LiteralPath (Join-Path $PSScriptRoot 'cad_inspect.cs') -Raw -Encoding UTF8) -ReferencedAssemblies @($interop,'System.Web.Extensions')
[CadReadOnlyInspect]::Run($AssemblyPath, $OutputPath)
