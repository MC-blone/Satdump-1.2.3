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

# ---- Detect plugins dir ----
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

Write-Output "Copying all vcpkg runtime DLLs from: $vcpkg_bin"
$vcpkg_dlls = Get-ChildItem $vcpkg_bin -File -Filter "*.dll"
foreach ($d in $vcpkg_dlls) {
    $dest = Join-Path portable $d.Name
    if (!(Test-Path $dest)) {
        Copy-Item $d.FullName portable
    }
}
Write-Output ("Copied {0} vcpkg DLLs (some may have been already present)" -f $vcpkg_dlls.Count)

# ---- dumpbin-based recursive resolution (best effort, optional) ----
function Parse-DumpBin($binary_path)
{
    $return_val = @()
    $dumpbin_result = & dumpbin /dependents $binary_path 2>$null
    if (-not $dumpbin_result) { return @() }
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

# Quick sanity check: is dumpbin usable?
$dumpbin_ok = $false
try {
    $test = & dumpbin 2>&1 | Select-Object -First 1
    if ($test -match 'Microsoft.*Dump') { $dumpbin_ok = $true }
} catch { }

if ($dumpbin_ok) {
    Write-Output "dumpbin is available; running recursive dependency resolution..."

    $input_dlls = @(
        Get-ChildItem portable -Recurse -File | Where-Object {
            $_.Extension -eq ".dll" -or $_.Extension -eq ".exe"
        }
    )

    $dll_array = @()
    foreach ($input_dll in $input_dlls) {
        $dll_array += Parse-DumpBin $input_dll.FullName
    }
    $dll_array = @($dll_array | Select-Object -Unique)

    $available_dlls = @()
    $available_dlls += Get-ChildItem $vcpkg_bin -File -Filter "*.dll"
    $available_dlls += Get-ChildItem $binary_path -File -Filter "*.dll"
    if ($plugin_path -ne $binary_path) {
        $available_dlls += Get-ChildItem $plugin_path -File -Filter "*.dll"
    }
    $available_dlls = @($available_dlls | Sort-Object FullName -Unique)

    $dll_map = @{}
    foreach ($available_dll in $available_dlls) {
        $dll_map[$available_dll.Name.ToLowerInvariant()] = $available_dll
    }

    $dlls_to_copy = @{}
    foreach ($req_dll in $dll_array) {
        $key = $req_dll.ToLowerInvariant()
        if ($dll_map.ContainsKey($key)) {
            $dlls_to_copy[$key] = $dll_map[$key]
        }
    }

    $processed = @{}
    while ($true) {
        $pending = @(
            $dlls_to_copy.Values | Where-Object {
                !$processed.ContainsKey($_.FullName.ToLowerInvariant())
            }
        )
        if ($pending.Count -eq 0) { break }
        foreach ($dll_to_copy in $pending) {
            $key = $dll_to_copy.FullName.ToLowerInvariant()
            $processed[$key] = $true
            $potential_dlls = Parse-DumpBin $dll_to_copy.FullName
            foreach ($req_dll in $potential_dlls) {
                $dll_key = $req_dll.ToLowerInvariant()
                if (!$dlls_to_copy.ContainsKey($dll_key) -and $dll_map.ContainsKey($dll_key)) {
                    $dlls_to_copy[$dll_key] = $dll_map[$dll_key]
                }
            }
        }
    }

    foreach ($dll_to_copy in $dlls_to_copy.Values) {
        $destination = Join-Path portable $dll_to_copy.Name
        if (!(Test-Path $destination)) {
            Copy-Item $dll_to_copy.FullName portable
        }
    }
    Write-Output ("Recursive pass added {0} file(s)" -f $dlls_to_copy.Count)
} else {
    Write-Output "WARNING: dumpbin not available; relying on full vcpkg bin copy only."
}

Write-Output "Portable package created successfully."

# ============================================================
# POST-BUILD INSPECTION
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
Write-Output ("Total: {0} binary file(s) in portable/" -f $portable_files.Count)
Write-Output "First 20:"
$portable_files | Select-Object -First 20 | ForEach-Object {
    $rel = $_.FullName.Substring($portable_root.Length).TrimStart('\','/')
    Write-Output ("  {0}  ({1:N0} bytes)" -f $rel, $_.Length)
}

# Sanity checks: these MUST exist for a working SatDump
$required = @("volk.dll", "fftw3f.dll", "hdf5.dll", "glfw3.dll", "libcurl.dll", "zstd.dll")
Write-Output ""
Write-Output "--- Required runtime DLL sanity check ---"
$missing_required = @()
foreach ($r in $required) {
    if (Test-Path (Join-Path $portable_root $r)) {
        Write-Output ("  OK       {0}" -f $r)
    } else {
        Write-Output ("  MISSING  {0}" -f $r)
        $missing_required += $r
    }
}

# Optional deep check with dumpbin
if ($dumpbin_ok) {
    Write-Output ""
    Write-Output "--- Deep dependency check (dumpbin) ---"
    $packaged_names = @{}
    foreach ($f in Get-ChildItem $portable_root -Recurse -File -Filter "*.dll") {
        $packaged_names[$f.Name.ToLowerInvariant()] = $true
    }
    $system_set = @{}
    @("kernel32.dll","user32.dll","gdi32.dll","advapi32.dll","shell32.dll",
      "ole32.dll","oleaut32.dll","ws2_32.dll","crypt32.dll","bcrypt.dll",
      "ncrypt.dll","winmm.dll","version.dll","shlwapi.dll","comdlg32.dll",
      "comctl32.dll","setupapi.dll","cfgmgr32.dll","dbghelp.dll","iphlpapi.dll",
      "dnsapi.dll","secur32.dll","netapi32.dll","userenv.dll","psapi.dll",
      "msvcrt.dll","ntdll.dll","ucrtbase.dll","mpr.dll","wldap32.dll",
      "wininet.dll","urlmon.dll","dwmapi.dll","uxtheme.dll","opengl32.dll",
      "glu32.dll","d3d11.dll","dxgi.dll","powrprof.dll","pdh.dll",
      "wtsapi32.dll","authz.dll","wevtapi.dll","rpcrt4.dll","oleacc.dll",
      "sspicli.dll","wintrust.dll","imm32.dll","msimg32.dll","dwrite.dll",
      "d2d1.dll","d3dcompiler_47.dll","hid.dll","winhttp.dll","mswsock.dll",
      "normaliz.dll","propsys.dll") | ForEach-Object { $system_set[$_] = $true }

    $missing_report = @{}
    foreach ($f in $portable_files) {
        $deps = @(Parse-DumpBin $f.FullName)
        foreach ($d in $deps) {
            $k = $d.ToLowerInvariant()
            if ($packaged_names.ContainsKey($k)) { continue }
            if ($system_set.ContainsKey($k))     { continue }
            if ($k -like "api-ms-win-*")         { continue }
            if ($k -like "ext-ms-*")             { continue }
            if (-not $missing_report.ContainsKey($k)) {
                $missing_report[$k] = @()
            }
            $rel = $f.FullName.Substring($portable_root.Length).TrimStart('\','/')
            $missing_report[$k] += $rel
        }
    }
    if ($missing_report.Count -eq 0) {
        Write-Output "  (none - all non-system dependencies are present)"
    } else {
        foreach ($k in ($missing_report.Keys | Sort-Object)) {
            Write-Output ("  [MISSING] {0,-40} required by: {1}" -f $k, ($missing_report[$k] -join ", "))
        }
    }
} else {
    Write-Output ""
    Write-Output "--- Deep dependency check skipped (dumpbin unavailable) ---"
}

if ($missing_required.Count -gt 0) {
    Write-Output ""
    Write-Output "============================================================"
    Write-Output ("ERROR: {0} required DLL(s) missing from portable/" -f $missing_required.Count)
    Write-Output ($missing_required -join ", ")
    Write-Output "============================================================"
    exit 1
}

Write-Output ""
Write-Output "============================================================"
Write-Output "INSPECTION DONE - portable package looks complete"
Write-Output "============================================================"