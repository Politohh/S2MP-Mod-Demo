<# Package the current DLL, launcher, and FFmpeg with numbered release docs. #>
param(
    [Parameter(Mandatory = $true)][string] $BasePackage,
    [Parameter(Mandatory = $true)][string] $OutputZip,
    [string] $ModDllPath,
    [string] $FFmpegDirectory
)

$ErrorActionPreference = 'Stop'
$sourceRoot = Split-Path $PSScriptRoot -Parent
$buildHeader = Get-Content -LiteralPath (Join-Path $sourceRoot 'src/ModBuild.hpp') -Raw
$build = [regex]::Match($buildHeader, 'constexpr int NUMBER\s*=\s*(\d+)')
if (-not $build.Success) { throw 'Cannot read the package build number.' }
if (-not $ModDllPath -and -not ([IO.Path]::GetFileName($BasePackage) -match "build-$($build.Groups[1].Value)\b")) {
    throw 'Base ZIP name must match the DLL package build number.'
}
if (-not ([IO.Path]::GetFileName($OutputZip) -match "build-$($build.Groups[1].Value)\b")) {
    throw 'Output ZIP name must contain the DLL package build number.'
}
if (Test-Path -LiteralPath $OutputZip) { throw "Output already exists: $OutputZip" }
if ($ModDllPath -and -not (Test-Path -LiteralPath $ModDllPath -PathType Leaf)) {
    throw "Mod DLL not found: $ModDllPath"
}
$requiredFFmpegFiles = @('ffmpeg.exe', 'FFmpeg/README.txt', 'FFmpeg/COPYING.LGPLv2.1', 'FFmpeg/source/ffmpeg-9.0.2.tar.xz')
if ($FFmpegDirectory) {
    $FFmpegDirectory = (Resolve-Path -LiteralPath $FFmpegDirectory).Path
    foreach ($file in $requiredFFmpegFiles) {
        if (-not (Test-Path -LiteralPath (Join-Path $FFmpegDirectory $file) -PathType Leaf)) {
            throw "Missing $file in FFmpeg bundle."
        }
    }
}

Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem

$source = [IO.Compression.ZipFile]::OpenRead((Resolve-Path -LiteralPath $BasePackage).Path)
try {
    $binaryNames = @('S2MP-Launcher.exe', 's2mp-mod.dll')
    foreach ($name in $binaryNames) {
        if ($name -eq 's2mp-mod.dll' -and $ModDllPath) { continue }
        if (-not $source.GetEntry($name)) { throw "Missing $name in base package." }
    }
    if (-not $FFmpegDirectory) {
        foreach ($file in $requiredFFmpegFiles) {
            if (-not $source.GetEntry($file)) { throw "Supply -FFmpegDirectory: $file is missing from the base package." }
        }
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
            if ($name -eq 's2mp-mod.dll' -and $ModDllPath) {
                Add-EntryBytes $output $name ([IO.File]::ReadAllBytes((Resolve-Path -LiteralPath $ModDllPath).Path))
                continue
            }
            $inputStream = $source.GetEntry($name).Open()
            $memory = [IO.MemoryStream]::new()
            try { $inputStream.CopyTo($memory); Add-EntryBytes $output $name $memory.ToArray() }
            finally { $memory.Dispose(); $inputStream.Dispose() }
        }

        if ($FFmpegDirectory) {
            foreach ($file in (Get-ChildItem -LiteralPath $FFmpegDirectory -Recurse -File | Sort-Object FullName)) {
                $relative = [IO.Path]::GetRelativePath($FFmpegDirectory, $file.FullName).Replace('\', '/')
                if ($relative -ne 'ffmpeg.exe' -and -not $relative.StartsWith('FFmpeg/')) { continue }
                Add-EntryBytes $output $relative ([IO.File]::ReadAllBytes($file.FullName))
            }
        }
        else {
            foreach ($entry in $source.Entries) {
                if ($entry.FullName -ne 'ffmpeg.exe' -and -not $entry.FullName.StartsWith('FFmpeg/')) { continue }
                if (-not $entry.Name) { continue }
                $inputStream = $entry.Open()
                $memory = [IO.MemoryStream]::new()
                try { $inputStream.CopyTo($memory); Add-EntryBytes $output $entry.FullName $memory.ToArray() }
                finally { $memory.Dispose(); $inputStream.Dispose() }
            }
        }

        $docs = [ordered]@{
            'INSTALL.txt'         = 'docs/INSTALL.txt'
            'README.md'           = 'docs/PACKAGE-README.md'
            'CREDITS.md'          = 'CREDITS.md'
            'RELEASE-NOTES.md'    = "docs/RELEASE-BUILD-$($build.Groups[1].Value).md"
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
