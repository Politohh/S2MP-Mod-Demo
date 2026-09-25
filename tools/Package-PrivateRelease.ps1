<# Repackage the two unchanged, numbered binaries with current release docs. #>
param(
    [Parameter(Mandatory = $true)][string] $BasePackage,
    [Parameter(Mandatory = $true)][string] $OutputZip
)

$ErrorActionPreference = 'Stop'
$sourceRoot = Split-Path $PSScriptRoot -Parent
$buildHeader = Get-Content -LiteralPath (Join-Path $sourceRoot 'src/ModBuild.hpp') -Raw
$build = [regex]::Match($buildHeader, 'constexpr int NUMBER\s*=\s*(\d+)')
if (-not $build.Success) { throw 'Cannot read the package build number.' }
if (-not ([IO.Path]::GetFileName($BasePackage) -match "build-$($build.Groups[1].Value)\b")) {
    throw 'Base ZIP name must match the DLL package build number.'
}
if (-not ([IO.Path]::GetFileName($OutputZip) -match "build-$($build.Groups[1].Value)\b")) {
    throw 'Output ZIP name must contain the DLL package build number.'
}
if (Test-Path -LiteralPath $OutputZip) { throw "Output already exists: $OutputZip" }

Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem

$source = [IO.Compression.ZipFile]::OpenRead((Resolve-Path -LiteralPath $BasePackage).Path)
try {
    $binaryNames = @('S2MP-Launcher.exe', 's2mp-mod.dll')
    foreach ($name in $binaryNames) {
        if (-not $source.GetEntry($name)) { throw "Missing $name in base package." }
    }

    $output = [IO.Compression.ZipFile]::Open($OutputZip, [IO.Compression.ZipArchiveMode]::Create)
    try {
        $hashLines = [Collections.Generic.List[string]]::new()

        function Add-EntryBytes([IO.Compression.ZipArchive] $archive, [string] $entryName, [byte[]] $bytes) {
            $entry = $archive.CreateEntry($entryName, [IO.Compression.CompressionLevel]::Optimal)
            $stream = $entry.Open()
            try { $stream.Write($bytes, 0, $bytes.Length) } finally { $stream.Dispose() }
            $digest = [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($bytes))
            $hashLines.Add("$digest  $entryName")
        }

        foreach ($name in $binaryNames) {
            $inputStream = $source.GetEntry($name).Open()
            $memory = [IO.MemoryStream]::new()
            try { $inputStream.CopyTo($memory); Add-EntryBytes $output $name $memory.ToArray() }
            finally { $memory.Dispose(); $inputStream.Dispose() }
        }

        $docs = [ordered]@{
            'INSTALL.txt'         = 'docs/INSTALL.txt'
            'README.md'           = 'README.md'
            'CREDITS.md'          = 'CREDITS.md'
            'RELEASE-NOTES.md'    = 'docs/RELEASE-BUILD-28.md'
            'RESHADE-SETUP.txt'   = 'docs/RESHADE-SETUP.txt'
            'ReShade-LICENSE.md'  = 'src/third_party/reshade/LICENSE.md'
        }
        foreach ($pair in $docs.GetEnumerator()) {
            $file = Join-Path $sourceRoot $pair.Value
            Add-EntryBytes $output $pair.Key ([IO.File]::ReadAllBytes($file))
        }
        $manifest = ($hashLines -join "`n") + "`n"
        $entry = $output.CreateEntry('SHA256SUMS.txt')
        $writer = [IO.StreamWriter]::new($entry.Open(), [Text.UTF8Encoding]::new($false))
        try { $writer.Write($manifest) } finally { $writer.Dispose() }
    }
    finally { $output.Dispose() }
}
finally { $source.Dispose() }

Write-Output "Created $OutputZip"
