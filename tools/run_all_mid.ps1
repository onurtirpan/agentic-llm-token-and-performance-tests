# Build, serve, conformance-test and log-check every mid-tier implementation.
# Usage:  pwsh -File tools\run_all_mid.ps1

$ErrorActionPreference = "Continue"
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

$env:Path = "C:\Scoop\apps\mingw\current\bin;" + $env:Path
$env:Path = $env:Path + ";C:\Scoop\apps\ruby\current\bin;C:\Scoop\apps\ruby\current\gems\bin"
$env:Path = $env:Path + ";C:\Scoop\apps\sbcl\current"
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
        [int]$WaitSeconds = 3,
        [scriptblock]$Before = $null
    )
    Stop-Port8080
    if ($Before) { & $Before }
    $out = "$env:TEMP\mid-$Name.out.txt"
    $err = "$env:TEMP\mid-$Name.err.txt"
    Remove-Item $out, $err -ErrorAction SilentlyContinue
    $process = Start-Process -FilePath $Exe -ArgumentList $Arguments -PassThru `
        -WindowStyle Hidden -RedirectStandardError $err -RedirectStandardOutput $out
    Start-Sleep -Seconds $WaitSeconds
    $cases = python tools\conformance_mid.py 2>&1 | Select-Object -Last 1
    Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
    Start-Sleep -Milliseconds 400
    $logs = python tools\check_logs.py $out 2>&1 | Select-Object -Last 1
    "{0,-12} {1,-20} {2}" -f $Name, $cases, $logs
}

Write-Host "=== build ===" -ForegroundColor Cyan
Push-Location impl-mid\typescript; npx tsc; Pop-Location
Push-Location impl-mid\go; go build -o taskservice.exe .; Pop-Location
Push-Location impl-mid\csharp; dotnet build -c Release -v q --nologo; Pop-Location
Push-Location impl-mid\rust; cargo build --release; Pop-Location
Push-Location impl-mid\java; & "C:\Scoop\apps\maven\current\bin\mvn.cmd" -q -B package -DskipTests; Pop-Location
Push-Location impl-mid\zig; zig build-exe main.zig -O ReleaseSafe; Pop-Location
Push-Location impl-mid\kotlin; & "C:\Scoop\apps\maven\current\bin\mvn.cmd" -q -B package -DskipTests; Pop-Location
# Dump an SBCL image with the dependencies in it. Loading them through Quicklisp
# on every boot costs 4.6 s of cold start; from the image it is 0.2 s.
sbcl --non-interactive --no-userinit --load tools\build_lisp_core.lisp "impl-mid/lisp/deps.core"
gcc -std=c11 -O2 -Wall -Wextra -static -o impl-mid\c\taskservice.exe impl-mid\c\main.c -lws2_32
g++ -std=c++20 -O2 -Wall -Wextra -static -o impl-mid\cpp\taskservice.exe impl-mid\cpp\main.cpp -lws2_32

Write-Host "`n=== conformance and logs ===" -ForegroundColor Cyan
Invoke-Case -Name "python" -Exe "python" -Arguments @("impl-mid\python\main.py") -WaitSeconds 5
Invoke-Case -Name "javascript" -Exe "node" -Arguments @("impl-mid\javascript\src\main.js")
Invoke-Case -Name "typescript" -Exe "node" -Arguments @("impl-mid\typescript\dist\main.js")
Invoke-Case -Name "csharp" -Exe ".\impl-mid\csharp\bin\Release\net10.0\csharp.exe" -WaitSeconds 5
Invoke-Case -Name "go" -Exe ".\impl-mid\go\taskservice.exe" -WaitSeconds 2
Invoke-Case -Name "php" -Exe "php" `
    -Arguments @("-S", "127.0.0.1:8080", "-t", "impl-mid\php\public", "impl-mid\php\public\index.php") `
    -Before { Remove-Item "impl-mid\php\store.json" -ErrorAction SilentlyContinue }
Invoke-Case -Name "ruby" -Exe "ruby" -Arguments @("impl-mid\ruby\main.rb") -WaitSeconds 4
Invoke-Case -Name "java" -Exe "$jdk\bin\java.exe" `
    -Arguments @("-jar", "impl-mid\java\target\taskservice-0.1.0.jar") -WaitSeconds 14
Invoke-Case -Name "kotlin" -Exe "$jdk\bin\java.exe" `
    -Arguments @("-jar", "impl-mid\kotlin\target\taskservice-0.1.0.jar") -WaitSeconds 14
Invoke-Case -Name "rust" -Exe ".\impl-mid\rust\target\release\taskservice.exe" -WaitSeconds 2
Invoke-Case -Name "zig" -Exe ".\impl-mid\zig\main.exe" -WaitSeconds 2
Invoke-Case -Name "lisp" -Exe "sbcl" `
    -Arguments @("--core", "impl-mid/lisp/deps.core", "--noinform", "--non-interactive",
                 "--no-userinit", "--load", "impl-mid/lisp/main.lisp") -WaitSeconds 3
Invoke-Case -Name "c" -Exe ".\impl-mid\c\taskservice.exe" -WaitSeconds 2
Invoke-Case -Name "cpp" -Exe ".\impl-mid\cpp\taskservice.exe" -WaitSeconds 2

Stop-Port8080
Write-Host "`n=== measure ===" -ForegroundColor Cyan
python tools\measure.py
