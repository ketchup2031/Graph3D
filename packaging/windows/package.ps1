#Requires -Version 5.1
[CmdletBinding()]
param(
    [Parameter(Mandatory)] [ValidatePattern('^\d+\.\d+\.\d+$')] [string]$Version,
    [string]$BuildDirectory = 'build/release',
    [string]$FFmpegDirectory = 'build/installers/ffmpeg-src/ffmpeg-9.0.2',
    [string]$TIFFDirectory = 'build/installers/tiff-src',
    [string]$MsysRoot = 'C:/msys64',
    [string]$InnoCompiler = "$env:LOCALAPPDATA/Programs/Inno Setup 6/ISCC.exe"
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$ucrt = (Resolve-Path (Join-Path $MsysRoot 'ucrt64')).Path
$ucrtBin = Join-Path $ucrt 'bin'
$prefix = '/ucrt64'
$bash = (Resolve-Path (Join-Path $MsysRoot 'usr/bin/bash.exe')).Path
$objdump = Join-Path $ucrtBin 'objdump.exe'
$compiler = (Get-Item -LiteralPath $InnoCompiler).FullName
$build = if ([IO.Path]::IsPathRooted($BuildDirectory)) { $BuildDirectory } else { Join-Path $repo $BuildDirectory }
$ffmpeg = if ([IO.Path]::IsPathRooted($FFmpegDirectory)) { $FFmpegDirectory } else { Join-Path $repo $FFmpegDirectory }
$tiff = if ([IO.Path]::IsPathRooted($TIFFDirectory)) { $TIFFDirectory } else { Join-Path $repo $TIFFDirectory }
$exe = Get-Item -LiteralPath (Join-Path $build 'Graph3D.exe')
$cache = Get-Content -LiteralPath (Join-Path $build 'CMakeCache.txt')
if ($cache -notcontains 'CMAKE_BUILD_TYPE:STRING=Release') {
    throw 'Configure the installer build with CMAKE_BUILD_TYPE=Release.'
}

# Use a fresh staging tree so DLLs removed from a build never linger in the installer.
$output = (New-Item -ItemType Directory -Force (Join-Path $repo 'build/installers')).FullName
$stage = (New-Item -ItemType Directory (Join-Path $output ('payload-' + [guid]::NewGuid().ToString('N')))).FullName
$absoluteOutput = [IO.Path]::GetFullPath($output).TrimEnd('\', '/')
try {
    $installedExe = Join-Path $stage 'Graph3D.exe'
    Copy-Item -LiteralPath $exe.FullName -Destination $installedExe
    $null = New-Item -ItemType Directory (Join-Path $stage 'licenses')
    Copy-Item -LiteralPath (Join-Path $repo 'THIRD_PARTY_NOTICES.md') -Destination $stage
    Copy-Item -LiteralPath (Join-Path $repo 'examples') -Destination $stage -Recurse

    $queue = [Collections.Generic.Queue[string]]::new()
    $bundled = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    $packageFiles = [Collections.Generic.List[string]]::new()
    $ffmpegFiles = [Collections.Generic.List[string]]::new()
    $includesTiff = $false
    $queue.Enqueue($installedExe)
    foreach ($plugin in @('platforms/qwindows.dll', 'styles/qmodernwindowsstyle.dll')) {
        $relative = "share/qt6/plugins/$plugin"
        $original = Join-Path $ucrt $relative
        if (!(Test-Path -LiteralPath $original) -and $plugin -ne 'platforms/qwindows.dll') { continue }
        $destination = Join-Path $stage $plugin
        $null = New-Item -ItemType Directory -Force (Split-Path $destination -Parent)
        Copy-Item -LiteralPath $original -Destination $destination
        $packageFiles.Add("$prefix/$relative")
        $queue.Enqueue($destination)
    }
    Set-Content -LiteralPath (Join-Path $stage 'qt.conf') -Value @('[Paths]', 'Plugins = .') -Encoding ascii

    while ($queue.Count) {
        $binary = $queue.Dequeue()
        $imports = & $objdump -p $binary
        if ($LASTEXITCODE -ne 0) { throw "Cannot inspect imports for $binary" }
        foreach ($line in $imports) {
            if ($line -notmatch 'DLL Name:\s+([\w.+-]+\.dll)\s*$') { continue }
            $name = $Matches[1]
            if ($name -match '^(api-ms-|ext-ms-)') { continue }
            if ($name -match '^libjbig') { throw 'The release runtime must not include JBIG-KIT.' }
            $tiffImport = $name -ieq 'libtiff-6.dll'
            $ffmpegImport = $name -match '^(avcodec|avformat|avutil|swresample|swscale)-\d+\.dll$'
            if ($ffmpegImport) {
                # Open CASCADE links FFmpeg through TKService. Use the LGPL build, not MSYS2's GPL variant.
                $original = Join-Path $ffmpeg "lib$($Matches[1])/$name"
                if (!(Test-Path -LiteralPath $original)) { throw "Build the LGPL FFmpeg runtime first; missing $original" }
            } elseif ($tiffImport) {
                $original = Join-Path $tiff "build/libtiff/$name"
                if (!(Test-Path -LiteralPath $original)) { throw "Build LibTIFF with JBIG disabled first; missing $original" }
            } else {
                $original = Join-Path $ucrtBin $name
            }
            if (!(Test-Path -LiteralPath $original)) {
                if (![IO.File]::Exists((Join-Path ([Environment]::SystemDirectory) $name))) {
                    throw "Missing dependency $name required by $binary"
                }
                continue
            }
            if ($bundled.Add($name)) {
                Copy-Item -LiteralPath $original -Destination $stage
                if ($ffmpegImport) { $ffmpegFiles.Add($name) }
                elseif ($tiffImport) { $includesTiff = $true }
                else { $packageFiles.Add("$prefix/bin/$name") }
                $queue.Enqueue((Join-Path $stage $name))
            }
        }
    }

    # pacman records the actual owner/version of every redistributed DLL.
    $fileArguments = $packageFiles -join ' '
    $owners = & $bash -lc "LC_ALL=C pacman -Qo $fileArguments"
    if ($LASTEXITCODE -ne 0) { throw 'Unable to resolve the packages owning the runtime files.' }
    $packages = @{}
    foreach ($line in $owners) {
        if ($line -notmatch '^(/ucrt64/\S+) is owned by (mingw-w64-ucrt-x86_64-\S+) (\S+)$') {
            throw "Could not parse package owner: $line"
        }
        $file, $package, $packageVersion = $Matches[1], $Matches[2], $Matches[3]
        if (!$packages.ContainsKey($package)) {
            $packages[$package] = @{ Version = $packageVersion; Files = [Collections.Generic.List[string]]::new() }
        }
        $packages[$package].Files.Add($file.Replace('/ucrt64/', ''))
    }

    $manifest = @('# Bundled Windows runtime', '', 'Graph3D dynamically loads these redistributable libraries. Windows system DLLs are not included.', '',
        'For each package, use the exact source recipe, patches and upstream source information in the linked MSYS2 source archive. The libraries can be replaced with compatible builds.', '')
    if ($ffmpegFiles.Count) {
        $ffmpegConfig = Get-Content -LiteralPath (Join-Path $ffmpeg 'config.h')
        foreach ($option in @('GPL', 'NONFREE', 'VERSION3')) {
            if ($ffmpegConfig -notcontains "#define CONFIG_$option 0") { throw "Unexpected FFmpeg configuration: $option" }
        }
        $licenseTarget = (New-Item -ItemType Directory (Join-Path $stage 'licenses/ffmpeg')).FullName
        foreach ($name in @('COPYING.LGPLv2.1', 'LICENSE.md')) {
            Copy-Item -LiteralPath (Join-Path $ffmpeg $name) -Destination $licenseTarget
        }
        $manifest += @('## FFmpeg 9.0.2 (LGPL 2.1 or later)',
            '- Source: https://ffmpeg.org/releases/ffmpeg-9.0.2.tar.xz',
            "- Build: https://github.com/ketchup2031/Graph3D/blob/v$Version/packaging/windows/build-ffmpeg.sh",
            ('- Files: ' + (($ffmpegFiles | Sort-Object) -join ', ')), '- Notices: licenses/ffmpeg', '')
    }
    if ($includesTiff) {
        $target = (New-Item -ItemType Directory (Join-Path $stage 'licenses/libtiff')).FullName
        Copy-Item -LiteralPath (Join-Path $tiff 'tiff-4.7.2/LICENSE.md') -Destination $target
        $manifest += @('## LibTIFF 4.7.2 (LibTIFF license)',
            '- Source: https://download.osgeo.org/libtiff/tiff-4.7.2.tar.gz',
            "- Build: https://github.com/ketchup2031/Graph3D/blob/v$Version/README.md#сборка-установщика",
            '- Files: libtiff-6.dll', '- Notices: licenses/libtiff', '')
    }
    $packageArguments = ($packages.Keys -join ' ')
    $packageListing = & $bash -lc "LC_ALL=C pacman -Ql $packageArguments"
    if ($LASTEXITCODE -ne 0) { throw 'Cannot list licenses for the runtime packages.' }
    $licenses = @{}
    foreach ($line in $packageListing) {
        if ($line -match '^(\S+) /ucrt64/(share/licenses/.+[^/])$') {
            $package, $file = $Matches[1], $Matches[2]
            if (!$licenses.ContainsKey($package)) { $licenses[$package] = [Collections.Generic.List[string]]::new() }
            $licenses[$package].Add($file)
        }
    }
    foreach ($package in ($packages.Keys | Sort-Object)) {
        $entry = $packages[$package]
        $packageVersion = $entry.Version
        $metadataPath = Join-Path $MsysRoot "var/lib/pacman/local/$package-$($packageVersion -replace '^\d+:','')/desc"
        $metadata = Get-Content -LiteralPath $metadataPath
        $baseIndex = [Array]::IndexOf($metadata, '%BASE%')
        if ($baseIndex -lt 0) { throw "Missing source package name for $package" }
        $base = $metadata[$baseIndex + 1]
        $sourceVersion = $packageVersion -replace '^\d+:', ''
        $source = "https://repo.msys2.org/mingw/sources/$base-$sourceVersion.src.tar.zst"
        $manifest += ('## {0} {1}' -f $package, $packageVersion)
        $manifest += "- Source: $source"
        $licenseIndex = [Array]::IndexOf($metadata, '%LICENSE%')
        if ($licenseIndex -ge 0) {
            $end = $licenseIndex + 1
            while ($end -lt $metadata.Count -and $metadata[$end]) { $end++ }
            if ($end -gt $licenseIndex + 1) {
                $manifest += '- Package license metadata: ' + ($metadata[($licenseIndex + 1)..($end - 1)] -join '; ')
            }
        }
        $manifest += '- Files: ' + (($entry.Files | Sort-Object) -join ', ')
        if ($licenses.ContainsKey($package)) {
            $licenseTarget = (New-Item -ItemType Directory -Force (Join-Path $stage "licenses/$package")).FullName
            foreach ($file in $licenses[$package]) {
                $relative = $file.Substring('share/licenses/'.Length)
                $destination = Join-Path $licenseTarget $relative
                $null = New-Item -ItemType Directory -Force (Split-Path $destination -Parent)
                Copy-Item -LiteralPath (Join-Path $ucrt $file) -Destination $destination
            }
            $manifest += "- Notices: licenses/$package"
        }
        $manifest += ''
    }
    $manifest | Set-Content -LiteralPath (Join-Path $stage 'THIRD_PARTY_RUNTIME.md') -Encoding utf8
    Copy-Item -LiteralPath (Join-Path $stage 'THIRD_PARTY_RUNTIME.md') -Destination (Join-Path $output "Graph3D-$Version-ThirdParty.md")

    & $compiler /Q "/DVersion=$Version" "/DPayloadFolder=$stage" "/DOutputFolder=$output" (Join-Path $PSScriptRoot 'Graph3D.iss')
    if ($LASTEXITCODE -ne 0) { throw 'Inno Setup failed to build the installer.' }
    $installer = Get-Item -LiteralPath (Join-Path $output "Graph3D-$Version-Windows-x64-Setup.exe")
    $hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $installer.FullName).Hash.ToLowerInvariant()
    "$hash *$($installer.Name)" | Set-Content -LiteralPath "$($installer.FullName).sha256" -Encoding ascii
    Write-Output $installer.FullName
} finally {
    $resolvedStage = [IO.Path]::GetFullPath((Resolve-Path -LiteralPath $stage).Path)
    if (!$resolvedStage.StartsWith("$absoluteOutput\", [StringComparison]::OrdinalIgnoreCase) -or
        (Split-Path $resolvedStage -Leaf) -notmatch '^payload-[a-f0-9]{32}$') {
        throw "Unexpected installer staging directory: $resolvedStage"
    }
    Remove-Item -LiteralPath $resolvedStage -Recurse -Force
}
