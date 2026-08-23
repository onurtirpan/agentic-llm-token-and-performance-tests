# Build and conformance-test every implementation.
# Usage:  pwsh -File tools\run_all.ps1

$ErrorActionPreference = "Continue"
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

$env:Path = "C:\Scoop\apps\mingw\current\bin;" + $env:Path
$jdk = "C:\Scoop\apps\temurin21-jdk\current"

function Stop-Port8080 {
    Get-NetTCPConnection -LocalPort 8080 -State Listen -ErrorAction SilentlyContinue |
        ForEach-Object { Stop-Process -Id $_.OwningProcess -Force -ErrorAction SilentlyContinue }
    Start-Sleep -Milliseconds 400
}

function Invoke-Case {
    param(
        [string]$Name,
        [string]$Exe,
        [string[]]$Arguments = @(),
        [int]$WaitSeconds = 3,
        [scriptblock]$Before = $null
    )
    Stop-Port8080
    if ($Before) { & $Before }
    $err = "$env:TEMP\lang-$Name.err.txt"
    $out = "$env:TEMP\lang-$Name.out.txt"
    $process = Start-Process -FilePath $Exe -ArgumentList $Arguments -PassThru `
        -WindowStyle Hidden -RedirectStandardError $err -RedirectStandardOutput $out
    Start-Sleep -Seconds $WaitSeconds
    $report = python tools\conformance.py 2>&1 | Select-Object -Last 1
    Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
    "{0,-12} {1}" -f $Name, $report
}

Write-Host "=== build ===" -ForegroundColor Cyan
Push-Location impl\typescript; npx tsc; Pop-Location
Push-Location impl\go; go build -o taskservice.exe .; Pop-Location
Push-Location impl\csharp; dotnet build -c Release -v q --nologo; Pop-Location
Push-Location impl\rust; cargo build --release; Pop-Location
Push-Location impl\java
$env:JAVA_HOME = $jdk
& "C:\Scoop\apps\maven\current\bin\mvn.cmd" -q -B package -DskipTests
Pop-Location
gcc -std=c11 -O2 -Wall -Wextra -static -o impl\c\taskservice.exe impl\c\main.c -lws2_32
g++ -std=c++20 -O2 -Wall -Wextra -static -o impl\cpp\taskservice.exe impl\cpp\main.cpp -lws2_32
Push-Location impl\zig; zig build-exe main.zig -O ReleaseSafe; Pop-Location

Write-Host "`n=== conformance ===" -ForegroundColor Cyan
Invoke-Case -Name "python" -Exe "python" -Arguments @("impl\python\main.py") -WaitSeconds 5
Invoke-Case -Name "typescript" -Exe "node" -Arguments @("impl\typescript\dist\main.js")
Invoke-Case -Name "csharp" -Exe ".\impl\csharp\bin\Release\net10.0\csharp.exe" -WaitSeconds 5
Invoke-Case -Name "go" -Exe ".\impl\go\taskservice.exe" -WaitSeconds 2
Invoke-Case -Name "php" -Exe "php" `
    -Arguments @("-S", "127.0.0.1:8080", "-t", "impl\php\public", "impl\php\public\index.php") `
    -Before { Remove-Item "impl\php\store.json" -ErrorAction SilentlyContinue }
Invoke-Case -Name "java" -Exe "$jdk\bin\java.exe" `
    -Arguments @("-jar", "impl\java\target\taskservice-0.1.0.jar") -WaitSeconds 14
Invoke-Case -Name "rust" -Exe ".\impl\rust\target\release\taskservice.exe" -WaitSeconds 2
Invoke-Case -Name "zig" -Exe ".\impl\zig\main.exe" -WaitSeconds 2
Invoke-Case -Name "c" -Exe ".\impl\c\taskservice.exe" -WaitSeconds 2
Invoke-Case -Name "cpp" -Exe ".\impl\cpp\taskservice.exe" -WaitSeconds 2

Stop-Port8080
Write-Host "`n=== measure ===" -ForegroundColor Cyan
python tools\measure.py
