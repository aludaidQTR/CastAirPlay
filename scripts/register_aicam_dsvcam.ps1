param(
    [string]$DllPath = 'build\native\Release\aicam_dsvcam.dll'
)

$ErrorActionPreference = 'Stop'
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = [Security.Principal.WindowsPrincipal]::new($identity)
if (!$principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'Run this installer step from an elevated 64-bit PowerShell window.'
}
if (![Environment]::Is64BitProcess) {
    throw 'Use 64-bit PowerShell to register the x64 AiCam filter.'
}

$resolvedDll = (Resolve-Path $DllPath).Path
$regsvr32 = Join-Path $env:WINDIR 'System32\regsvr32.exe'
$process = Start-Process -FilePath $regsvr32 `
    -ArgumentList @('/s', "`"$resolvedDll`"") `
    -Wait -PassThru
if ($process.ExitCode -ne 0) {
    throw "regsvr32 failed with exit code $($process.ExitCode)."
}

$clsid = '{8E6E5F87-1AA3-4AB4-8D17-91640B7DA231}'
$inproc = "Registry::HKEY_LOCAL_MACHINE\SOFTWARE\Classes\CLSID\$clsid\InprocServer32"
if (!(Test-Path $inproc)) {
    throw 'Registration returned success but the per-machine x64 COM key was not found.'
}
$registeredPath = (Get-ItemProperty $inproc).'(default)'
if (![string]::Equals($registeredPath, $resolvedDll, [StringComparison]::OrdinalIgnoreCase)) {
    throw "Registered DLL path does not match: $registeredPath"
}
Write-Host "Registered x64 AiCam DirectShow filter per-machine: $resolvedDll"
