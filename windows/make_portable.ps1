param(
    [string]$source_path,
    [string]$triplet="x64-windows"
)

if ([string]::IsNullOrWhiteSpace($source_path)) {
    $source_path = Join-Path $PSScriptRoot ".."
}
$source_path = (Resolve-Path $source_path).Path
$build_path = (Get-Location).Path
$binary_path = Join-Path $build_path "Release"

if (!(Test-Path "$binary_path\satdump.exe")) {
    $binary_path = $build_path
}

# ---- Detect plugins dir: handle VS multi-config layouts ----
$config = "Release"
$plugin_candidates = @(
    (Join-Path $binary_path "plugins"),
    (Join-Path $binary_path "plugins\$config"),
    (Join-Path $build_path "plugins\$config"),
    (Join-Path $build_path "$config\plugins"),
    (Join-Path $build_path "plugins")
) | Select-Object -Unique

$plugin_path = $plugin_candidates | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $plugin_path) {
    throw "Cannot find plugins directory. Tried: $($plugin_candidates -join '; ')"
}
Write-Output "Using plugins directory: $plugin_path"
# ------------------------------------------------------

$vcpkg_bin = Join-Path $source_path "vcpkg\installed\$triplet\bin"

if (!(Test-Path "$binary_path\satdump.exe")) {
    throw "Cannot find satdump.exe"
}
if (!(Test-Path "$binary_path\satdump-ui.exe")) {
    throw "Cannot find satdump-ui.exe"
}
if (!(Test-Path $vcpkg_bin)) {
    throw "Cannot find vcpkg DLL directory: $vcpkg_bin"
}

if (Test-Path portable) {
    Remove-Item portable -Recurse -Force
}
New-Item -ItemType Directory portable | Out-Null

Copy-Item "$binary_path\satdump.exe" portable
Copy-Item "$binary_path\satdump-ui.exe" portable
Get-ChildItem $binary_path -File -Filter "*.dll" | Copy-Item -Destination portable

New-Item -ItemType Directory "portable\plugins" | Out-Null
Get-ChildItem $plugin_path -File -Filter "*.dll" | Copy-Item -Destination "portable\plugins"

Copy-Item "$source_path\resources" portable -Recurse
Copy-Item "$source_path\satdump_cfg.json" portable

function Parse-DumpBin($binary_path)
{
    $return_val = @()
    $dumpbin_result = & dumpbin /dependents $binary_path 2>$null
    $reading = $false

    foreach ($line in $dumpbin_result)
    {
        $this_line = $line.Trim()

        if ($this_line -eq "Image has the following dependencies:")
        {
            $reading = $true
            continue
        }

        if ($reading -and [string]::IsNullOrWhiteSpace($this_line))
        {
            break
        }

        if ($reading -and $this_line -match '^[A-Za-z0-9_.+-]+\.dll$')
        {
            $return_val += $this_line
        }
    }

    return $return_val
}

$input_dlls = @(
    Get-ChildItem portable -Recurse -File | Where-Object {
        $_.Extension -eq ".dll" -or $_.Extension -eq ".exe"
    }
)

$dll_array = @()

Write-Output "Input DLLs/EXE..."

foreach ($input_dll in $input_dlls)
{
    Write-Output $input_dll.FullName
    $dll_array += Parse-DumpBin $input_dll.FullName
}

$dll_array = @($dll_array | Select-Object -Unique)

Write-Output "Required DLLs..."

foreach ($req_dll in $dll_array)
{
    Write-Output $req_dll
}

$available_dlls = @()
$available_dlls += Get-ChildItem $vcpkg_bin -File -Filter "*.dll"
$available_dlls += Get-ChildItem $binary_path -File -Filter "*.dll"
if ($plugin_path -ne $binary_path) {
    $available_dlls += Get-ChildItem $plugin_path -File -Filter "*.dll"
}

$available_dlls = @(
    $available_dlls | Sort-Object FullName -Unique
)

Write-Output "Available DLLs..."

$dll_map = @{}
foreach ($available_dll in $available_dlls)
{
    $dll_map[$available_dll.Name.ToLowerInvariant()] = $available_dll
    Write-Output $available_dll.FullName
}

$dlls_to_copy = @{}
foreach ($req_dll in $dll_array)
{
    $key = $req_dll.ToLowerInvariant()

    if ($dll_map.ContainsKey($key))
    {
        $dlls_to_copy[$key] = $dll_map[$key]
    }
}

Write-Output "Recursively resolving dependencies..."

$processed = @{}

while ($true)
{
    $pending = @(
        $dlls_to_copy.Values | Where-Object {
            !$processed.ContainsKey($_.FullName.ToLowerInvariant())
        }
    )

    if ($pending.Count -eq 0)
    {
        break
    }

    foreach ($dll_to_copy in $pending)
    {
        $key = $dll_to_copy.FullName.ToLowerInvariant()
        $processed[$key] = $true

        $potential_dlls = Parse-DumpBin $dll_to_copy.FullName

        foreach ($req_dll in $potential_dlls)
        {
            $dll_key = $req_dll.ToLowerInvariant()

            if (!$dlls_to_copy.ContainsKey($dll_key) -and $dll_map.ContainsKey($dll_key))
            {
                $dlls_to_copy[$dll_key] = $dll_map[$dll_key]
            }
        }
    }
}

Write-Output "Copying dependencies..."

foreach ($dll_to_copy in $dlls_to_copy.Values)
{
    $destination = Join-Path portable $dll_to_copy.Name

    if (!(Test-Path $destination))
    {
        Write-Output $dll_to_copy.FullName
        Copy-Item $dll_to_copy.FullName portable
    }
}

Write-Output "Portable package created successfully."

# ============================================================
# POST-BUILD INSPECTION (ASCII only, safe for PS 5.1 / ANSI)
# ============================================================

Write-Output ""
Write-Output "============================================================"
Write-Output "POST-BUILD INSPECTION"
Write-Output "============================================================"

$portable_root = (Resolve-Path "portable").Path
$portable_files = @(
    Get-ChildItem $portable_root -Recurse -File | Where-Object {
        $_.Extension -eq ".dll" -or $_.Extension -eq ".exe"
    } | Sort-Object FullName
)

Write-Output ""
Write-Output "--- Packaged binaries (in portable/) ---"
Write-Output ("{0,-8} {1,12}  {2}" -f "TYPE", "SIZE", "RELATIVE PATH")
Write-Output ("{0,-8} {1,12}  {2}" -f "----", "----", "-------------")

foreach ($f in $portable_files) {
    $rel = $f.FullName.Substring($portable_root.Length).TrimStart('\','/')
    $type = $f.Extension.TrimStart('.').ToUpperInvariant()
    Write-Output ("{0,-8} {1,12:N0}  {2}" -f $type, $f.Length, $rel)
}

Write-Output ""
Write-Output ("Total: {0} file(s) in portable/" -f $portable_files.Count)

# ---- Reverse check: every binary's deps present in portable/? ----
Write-Output ""
Write-Output "--- Dependency check (missing from portable/) ---"

$packaged_names = @{}
foreach ($f in Get-ChildItem $portable_root -Recurse -File -Filter "*.dll") {
    $packaged_names[$f.Name.ToLowerInvariant()] = $true
}

# System DLL allowlist: provided by Windows, do not package
$system_dlls = @(
    "kernel32.dll","user32.dll","gdi32.dll","advapi32.dll","shell32.dll",
    "ole32.dll","oleaut32.dll","ws2_32.dll","crypt32.dll","bcrypt.dll",
    "ncrypt.dll","winmm.dll","version.dll","shlwapi.dll","comdlg32.dll",
    "comctl32.dll","setupapi.dll","cfgmgr32.dll","dbghelp.dll","iphlpapi.dll",
    "dnsapi.dll","secur32.dll","netapi32.dll","userenv.dll","psapi.dll",
    "msvcrt.dll","ntdll.dll","api-ms-win-crt-runtime-l1-1-0.dll",
    "vcruntime140.dll","vcruntime140_1.dll","msvcp140.dll","concrt140.dll",
    "ucrtbase.dll","mpr.dll","wldap32.dll","wininet.dll","urlmon.dll",
    "dwmapi.dll","uxtheme.dll","opengl32.dll","glu32.dll","d3d11.dll",
    "dxgi.dll","powrprof.dll","pdh.dll","wtsapi32.dll","authz.dll",
    "wevtapi.dll","rpcrt4.dll","oleacc.dll","sspicli.dll","wintrust.dll",
    "imm32.dll","msimg32.dll","dwrite.dll","d2d1.dll","d3dcompiler_47.dll",
    "hid.dll","winhttp.dll","mswsock.dll","normaliz.dll","propsys.dll"
)
$system_set = @{}
foreach ($s in $system_dlls) { $system_set[$s] = $true }

$any_missing = $false
$missing_report = @{}

foreach ($f in $portable_files) {
    $deps = @(Parse-DumpBin $f.FullName)
    $missing = @()
    foreach ($d in $deps) {
        $k = $d.ToLowerInvariant()
        if ($packaged_names.ContainsKey($k)) { continue }
        if ($system_set.ContainsKey($k))     { continue }
        if ($k -like "api-ms-win-*")         { continue }
        if ($k -like "ext-ms-*")             { continue }
        $missing += $d
    }
    if ($missing.Count -gt 0) {
        $any_missing = $true
        $rel = $f.FullName.Substring($portable_root.Length).TrimStart('\','/')
        Write-Output ""
        Write-Output ("  {0}" -f $rel)
        foreach ($m in $missing) {
            Write-Output ("      [MISSING] {0}" -f $m)
            $mk = $m.ToLowerInvariant()
            if (-not $missing_report.ContainsKey($mk)) {
                $missing_report[$mk] = @()
            }
            $missing_report[$mk] += $rel
        }
    }
}

if (-not $any_missing) {
    Write-Output "  (none - all non-system dependencies are present)"
} else {
    Write-Output ""
    Write-Output "--- Summary: distinct missing DLLs ---"
    foreach ($k in ($missing_report.Keys | Sort-Object)) {
        $users = $missing_report[$k] -join ", "
        Write-Output ("  {0,-32} required by: {1}" -f $k, $users)
    }
    Write-Output ""
    Write-Output "HINT: copy these DLLs from the build/vcpkg install dirs into portable/,"
    Write-Output "      or add their directories to the `$available_dlls search path."
}

# ---- Extra: source-side DLLs not packaged (candidates for manual copy) ----
Write-Output ""
Write-Output "--- Source-side DLLs NOT packaged (candidates for manual copy) ---"

$src_dlls = @()
$search_roots = @($source_path, $build_path) | Select-Object -Unique
foreach ($root in $search_roots) {
    if (-not (Test-Path $root)) { continue }
    $src_dlls += Get-ChildItem $root -Recurse -File -Filter "*.dll" -ErrorAction SilentlyContinue
}

$packaged_full = @{}
foreach ($f in Get-ChildItem $portable_root -Recurse -File -Filter "*.dll") {
    $packaged_full[$f.Name.ToLowerInvariant()] = $true
}

$unpackaged = @()
foreach ($d in $src_dlls) {
    $k = $d.Name.ToLowerInvariant()
    if ($packaged_full.ContainsKey($k)) { continue }
    if ($system_set.ContainsKey($k))    { continue }
    $interesting = $false
    if ($missing_report.ContainsKey($k)) { $interesting = $true }
    if ($k -match '^(volk|libvolk|fftw|sndfile|soxr|zstd|lz4|hdf5|airspy|hackrf|limesuite|rtlsdr|bladerf|uhd|soapy)') {
        $interesting = $true
    }
    if ($interesting) {
        $unpackaged += $d
    }
}

if ($unpackaged.Count -eq 0) {
    Write-Output "  (none found - library may not have been built at all; check build step)"
} else {
    foreach ($d in ($unpackaged | Sort-Object FullName -Unique)) {
        Write-Output ("  {0}  ({1:N0} bytes)" -f $d.FullName, $d.Length)
    }
}

Write-Output ""
Write-Output "============================================================"
Write-Output "INSPECTION DONE"
Write-Output "============================================================"