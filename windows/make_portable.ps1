```powershell
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

$plugin_path = Join-Path $binary_path "plugins"
$vcpkg_bin = Join-Path $source_path "vcpkg\installed\$triplet\bin"

if (!(Test-Path "$binary_path\satdump.exe")) {
    throw "Cannot find satdump.exe"
}
if (!(Test-Path "$binary_path\satdump-ui.exe")) {
    throw "Cannot find satdump-ui.exe"
}
if (!(Test-Path $plugin_path)) {
    throw "Cannot find plugins directory: $plugin_path"
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
```