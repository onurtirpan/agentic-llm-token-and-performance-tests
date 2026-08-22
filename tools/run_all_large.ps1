# Build, serve, conformance-test and log-check every large-tier implementation.
# Usage:  pwsh -File tools\run_all_large.ps1

$ErrorActionPreference = "Continue"
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

$env:Path = "C:\Scoop\apps\mingw\current\bin;" + $env:Path
$jdk = "C:\Scoop\apps\temurin21-jdk\current"
$env:JAVA_HOME = $jdk

function Stop-Port8080 {
    Get-NetTCPConnection -LocalPort 8080 -State Listen -ErrorAction SilentlyContinue |
        ForEach-Object { Stop-Process -Id $_.OwningProcess -Force -ErrorAction SilentlyContinue }
    Start-Sleep -Milliseconds 500
}

function Invoke-Case {
    param(
        [string]$Name,
        [string]$Exe,
        [string[]]$Arguments = @(),
        [string]$WorkDir = $null,
        [int]$WaitSeconds = 3,
        [scriptblock]$Before = $null
    )
    Stop-Port8080
    if ($Before) { & $Before }
    $out = "$env:TEMP\large-$Name.out.txt"
    $err = "$env:TEMP\large-$Name.err.txt"
    Remove-Item $out, $err -ErrorAction SilentlyContinue
    $params = @{
        FilePath = $Exe; PassThru = $true; WindowStyle = "Hidden"
        RedirectStandardError = $err; RedirectStandardOutput = $out
    }
    if ($Arguments.Count -gt 0) { $params.ArgumentList = $Arguments }
    if ($WorkDir) { $params.WorkingDirectory = $WorkDir }
    $process = Start-Process @params
    Start-Sleep -Seconds $WaitSeconds
    $cases = python tools\conformance_large.py 2>&1 | Select-Object -Last 1
    Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
    Start-Sleep -Milliseconds 400
    $logs = python tools\check_logs.py $out 2>&1 | Select-Object -Last 1
    "{0,-12} {1,-22} {2}" -f $Name, $cases, $logs
}

Write-Host "=== build ===" -ForegroundColor Cyan
Push-Location impl-large\typescript; npx tsc; Pop-Location
Push-Location impl-large\go; go build -o taskservice.exe .; Pop-Location
Push-Location impl-large\csharp; dotnet build -c Release -v q --nologo; Pop-Location
Push-Location impl-large\rust; cargo build --release; Pop-Location
Push-Location impl-large\java; & "C:\Scoop\apps\maven\current\bin\mvn.cmd" -q -B package -DskipTests; Pop-Location
Push-Location impl-large\zig; zig build-exe main.zig -O ReleaseSafe; Pop-Location
gcc -std=c11 -O2 -Wall -Wextra -static -o impl-large\c\taskservice.exe `
    impl-large\c\api.c impl-large\c\service.c impl-large\c\store.c impl-large\c\domain.c -lws2_32
g++ -std=c++20 -O2 -Wall -Wextra -static -o impl-large\cpp\taskservice.exe `
    impl-large\cpp\api.cpp impl-large\cpp\service.cpp impl-large\cpp\store.cpp `
    impl-large\cpp\domain.cpp -lws2_32

Write-Host "`n=== conformance and logs ===" -ForegroundColor Cyan
Invoke-Case -Name "python" -Exe "python" -Arguments @("api.py") `
    -WorkDir "$root\impl-large\python" -WaitSeconds 6
Invoke-Case -Name "typescript" -Exe "node" -Arguments @("impl-large\typescript\dist\api.js")
Invoke-Case -Name "csharp" -Exe ".\impl-large\csharp\bin\Release\net10.0\csharp.exe" -WaitSeconds 5
Invoke-Case -Name "go" -Exe ".\impl-large\go\taskservice.exe" -WaitSeconds 2
Invoke-Case -Name "php" -Exe "php" `
    -Arguments @("-S", "127.0.0.1:8080", "-t", "impl-large\php\public", "impl-large\php\public\index.php") `
    -Before { Remove-Item "impl-large\php\store.json" -ErrorAction SilentlyContinue }
Invoke-Case -Name "php-bare" -Exe "php" `
    -Arguments @("-S", "127.0.0.1:8080", "-t", "impl-large\php-bare\public", "impl-large\php-bare\public\index.php") `
    -Before { Remove-Item "impl-large\php-bare\store.json" -ErrorAction SilentlyContinue }
Invoke-Case -Name "java" -Exe "$jdk\bin\java.exe" `
    -Arguments @("-jar", "impl-large\java\target\taskservice-0.1.0.jar") -WaitSeconds 14
Invoke-Case -Name "rust" -Exe ".\impl-large\rust\target\release\taskservice.exe" -WaitSeconds 2
Invoke-Case -Name "zig" -Exe ".\impl-large\zig\main.exe" -WaitSeconds 2
Invoke-Case -Name "c" -Exe ".\impl-large\c\taskservice.exe" -WaitSeconds 2
Invoke-Case -Name "cpp" -Exe ".\impl-large\cpp\taskservice.exe" -WaitSeconds 2

Stop-Port8080
Write-Host "`n=== measure ===" -ForegroundColor Cyan
python tools\measure.py
