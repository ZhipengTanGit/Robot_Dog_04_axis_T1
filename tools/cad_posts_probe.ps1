param([Parameter(Mandatory=$true)][string]$AssemblyPath)
$ErrorActionPreference='Stop'
$interop='D:\04_Mechanical_App\SOLIDWORKS2026\SOLIDWORKS\api\redist\SolidWorks.Interop.sldworks.dll'
Add-Type -Path $interop
Add-Type -TypeDefinition (Get-Content -LiteralPath (Join-Path $PSScriptRoot 'cad_feature_probe.cs') -Raw -Encoding UTF8) -ReferencedAssemblies @($interop,'System.Web.Extensions')
[CadFeatureProbe]::Posts($AssemblyPath, (Join-Path $PSScriptRoot 'cad-output\regions-complete\model.json'), (Join-Path $PSScriptRoot 'cad-output\posts-probe.json'))
