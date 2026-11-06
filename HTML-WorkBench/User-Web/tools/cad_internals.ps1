param([Parameter(Mandatory=$true)][string]$AssemblyPath)
$ErrorActionPreference='Stop'
if ($PSVersionTable.PSEdition -ne 'Desktop') { throw 'Run with Windows PowerShell 5.1.' }
$interop='D:\04_Mechanical_App\SOLIDWORKS2026\SOLIDWORKS\api\redist\SolidWorks.Interop.sldworks.dll'
Add-Type -Path $interop
Add-Type -TypeDefinition (Get-Content -LiteralPath (Join-Path $PSScriptRoot 'cad_internals.cs') -Raw -Encoding UTF8) -ReferencedAssemblies @($interop,'System.Web.Extensions')
[CadInternalExport]::Run($AssemblyPath, (Join-Path $PSScriptRoot 'cad-output'))
